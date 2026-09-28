/**
 * @brief SinkRegistry (ADR-003 Decision 4, issue #67): handles, quiescent removal, self-removal,
 *        mutual cross-removal without deadlock, slot recycling, and no post-destruction access.
 */
import std;
import speclab;
import mddlog.adapter.sinkregistry;

namespace {

using mddlog::adapter::SinkRegistry;

using CountingRegistry = SinkRegistry<void()>;

/**
 * @brief Join @p thread within @p bound, or fail the process outright instead of hanging.
 *
 * std::thread::join() has no timeout, so waiting on it directly would hang CI forever if the
 * registry actually deadlocked - exactly the failure mode these tests exist to catch. Waiting on
 * a future set at the end of the thread's own work instead lets a passing run join normally
 * within the bound. Detaching past the bound would leave the thread free to keep touching the
 * test's stack locals (the registry, captured references) after this function - and the whole
 * scenario - returns: that is undefined behavior on top of the failure being reported, and can
 * turn one flaky test into a crash in some unrelated later one. Aborting the process immediately
 * is the safe failure here: the thread never gets a chance to run past locals that no longer
 * exist, and the test binary reports a hard, unambiguous failure instead of a hang.
 */
void joinWithinBoundOrAbort(std::thread& thread, std::future<void>& completion, std::chrono::seconds bound, std::string_view what) {
    if (completion.wait_for(bound) == std::future_status::ready) {
        thread.join();
        return;
    }
    std::cerr << std::format("SinkRegistrySpec: {} did not complete within {}; aborting instead of hanging or detaching\n", what, bound);
    std::abort();
}

const speclab::Register singleAddReturnsOneHandle{"SinkRegistry: add() of a single callback returns one handle, unchanged in shape", "unit", [] {
                                                      return speclab::Test("sink-registry-single-add-returns-handle")
                                                          .Then("the handle is valid and the callback is invoked on emit()",
                                                                [] {
                                                                    speclab::core::Checks checks;
                                                                    CountingRegistry      registry;
                                                                    int                   calls = 0;

                                                                    const auto handle = registry.add([&] {
                                                                        ++calls;
                                                                    });
                                                                    checks.expect(handle.valid(), "add() returns a valid handle");
                                                                    checks.expect(registry.activeCount() == 1, "one callback is registered");

                                                                    registry.emit();
                                                                    checks.expect(calls == 1, "the callback was invoked once");

                                                                    registry.remove(handle);
                                                                    registry.emit();
                                                                    checks.expect(calls == 1, "no further invocation happens after removal");
                                                                    checks.expect(registry.activeCount() == 0, "the registry reports no live callbacks");
                                                                    checks.raise();
                                                                })
                                                          .Execute();
                                                  }};

const speclab::Register multiAddReturnsArrayOfHandles{
    "SinkRegistry: add() of several callbacks returns one handle per callback, each independently removable",
    "unit",
    [] {
        return speclab::Test("sink-registry-multi-add-returns-handle-array")
            .Then("every handle is valid, distinct, and removing one leaves the others intact - fixing the "
                  "size()-1 defect where only the last id of a multi-registration was recoverable",
                  [] {
                      speclab::core::Checks checks;
                      CountingRegistry      registry;
                      int                   a = 0;
                      int                   b = 0;
                      int                   c = 0;

                      auto handles = registry.add(
                          [&] {
                              ++a;
                          },
                          [&] {
                              ++b;
                          },
                          [&] {
                              ++c;
                          });
                      static_assert(std::same_as<decltype(handles), std::array<CountingRegistry::Handle, 3>>);

                      checks.expect(handles[0].valid() && handles[1].valid() && handles[2].valid(), "all three handles are valid");
                      checks.expect(!(handles[0] == handles[1]) && !(handles[1] == handles[2]) && !(handles[0] == handles[2]),
                                    "the three handles are pairwise distinct");
                      checks.expect(registry.activeCount() == 3, "all three callbacks are registered");

                      registry.emit();
                      checks.expect(a == 1 && b == 1 && c == 1, "all three callbacks were invoked");

                      // Removing the FIRST handle is exactly what the old fold-and-push_back API could not
                      // do: it only ever returned the last id, making every earlier registration unremovable.
                      registry.remove(handles[0]);
                      registry.emit();
                      checks.expect(a == 1 && b == 2 && c == 2, "only the removed callback stops receiving emissions");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register registrationRacesWithEmission{"SinkRegistry: Registration and removal race with emission without corrupting the snapshot", "unit", [] {
                                                          return speclab::Test("sink-registry-registration-races-with-emission")
                                                              .Then("many threads adding, removing and emitting concurrently complete cleanly",
                                                                    [] {
                                                                        speclab::core::Checks     checks;
                                                                        CountingRegistry          registry;
                                                                        std::atomic<std::int64_t> totalInvocations{0};
                                                                        std::atomic<bool>         stop{false};
                                                                        constexpr int             kAdders   = 4;
                                                                        constexpr int             kEmitters = 4;
                                                                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};

                                                                        std::vector<std::thread> threads;
                                                                        threads.reserve(kAdders + kEmitters);
                                                                        for (int i = 0; i < kAdders; ++i) {
                                                                            threads.emplace_back([&] {
                                                                                while (std::chrono::steady_clock::now() < deadline) {
                                                                                    auto h = registry.add([&] {
                                                                                        totalInvocations.fetch_add(1, std::memory_order_relaxed);
                                                                                    });
                                                                                    std::this_thread::yield();
                                                                                    registry.remove(h);
                                                                                }
                                                                            });
                                                                        }
                                                                        for (int i = 0; i < kEmitters; ++i) {
                                                                            threads.emplace_back([&] {
                                                                                while (!stop.load(std::memory_order_acquire))
                                                                                    registry.emit();
                                                                            });
                                                                        }

                                                                        std::this_thread::sleep_until(deadline);
                                                                        stop.store(true, std::memory_order_release);
                                                                        for (auto& t : threads)
                                                                            t.join();

                                                                        checks.expect(registry.activeCount() == 0,
                                                                                      "every added callback was removed by its own adder thread");
                                                                        checks.raise();
                                                                    })
                                                              .Execute();
                                                      }};

const speclab::Register removalWaitsForInFlightInvocation{
    "SinkRegistry: External removal blocks until an already-engaged invocation of that callback returns",
    "unit",
    [] {
        return speclab::Test("sink-registry-removal-waits-for-in-flight-invocation")
            .Then("remove() does not return before the blocking callback does",
                  [] {
                      speclab::core::Checks checks;
                      CountingRegistry      registry;
                      std::latch            entered{1};
                      std::latch            release{1};
                      std::atomic<bool>     callbackReturned{false};
                      std::atomic<bool>     removeReturned{false};

                      const auto handle = registry.add([&] {
                          entered.count_down();
                          release.wait();
                          callbackReturned.store(true, std::memory_order_release);
                      });

                      std::thread emitter([&] {
                          registry.emit();
                      });
                      entered.wait();

                      std::thread remover([&] {
                          registry.remove(handle);
                          removeReturned.store(true, std::memory_order_release);
                      });

                      // The callback is blocked on `release`; remove() must still be blocked too.
                      std::this_thread::sleep_for(std::chrono::milliseconds{200});
                      checks.expect(!removeReturned.load(std::memory_order_acquire), "remove() has not returned while its callback is still in flight");

                      release.count_down();
                      remover.join();
                      emitter.join();

                      checks.expect(callbackReturned.load(std::memory_order_acquire), "the callback ran to completion");
                      checks.expect(removeReturned.load(std::memory_order_acquire), "remove() returned once the callback finished");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register removerIgnoresSlotReusedAfterItsOwnGenerationFinalizes{
    "SinkRegistry: remove() is not confused by a different callback that reuses its just-retired slot",
    "unit",
    [] {
        return speclab::Test("sink-registry-remover-ignores-reused-slot")
            .Then("remove() stops waiting once its own generation is quiescent, even if the free list hands "
                  "the exact same slot to a new, busy registration before the waiting thread wakes up",
                  [] {
                      speclab::core::Checks checks;
                      CountingRegistry      registry;

                      // A handful of spinning noise threads for the duration of this scenario, not
                      // one per core: the race below depends on the remover thread NOT being
                      // rescheduled promptly after its wait() is notified, and some extra
                      // contention makes that wake-and-reschedule latency less predictable than on
                      // an otherwise idle machine. Oversubscribing every core (as an earlier version
                      // of this test did) made thread creation itself pathologically slow on a
                      // constrained CI runner (observed hanging past a two-minute ctest timeout on
                      // Windows and under TSan's own heavy instrumentation) without actually being
                      // needed to reproduce the race locally - four is enough to add contention
                      // without starving the runner.
                      constexpr unsigned       kNoiseThreads = 4;
                      std::atomic<bool>        stopNoise{false};
                      std::vector<std::thread> noiseThreads;
                      noiseThreads.reserve(kNoiseThreads);
                      for (unsigned n = 0; n < kNoiseThreads; ++n) {
                          noiseThreads.emplace_back([&stopNoise] {
                              while (!stopNoise.load(std::memory_order_relaxed)) {}
                          });
                      }

                      // Whether the free list hands the retired slot to the second registration
                      // before or after the remover thread wakes up and re-checks depends on OS
                      // scheduling, not on anything this test controls directly. Repeating the whole
                      // sequence for a bounded time budget instead of a fixed iteration count, racing
                      // as tightly as each attempt can, gives the unfavorable interleaving many
                      // chances to occur instead of depending on winning it once, while keeping
                      // total runtime predictable regardless of how slow thread creation is on a
                      // given platform (a fixed count of 5000 attempts took over two minutes under
                      // Windows and TSan, and was still killed by the CI timeout before finishing).
                      // This is a stress test, not a single deterministic reproduction.
                      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
                      while (std::chrono::steady_clock::now() < deadline) {
                          std::latch        firstEntered{1};
                          std::latch        firstRelease{1};
                          std::atomic<bool> removeReturned{false};

                          const auto handle = registry.add([&] {
                              firstEntered.count_down();
                              firstRelease.wait();
                          });

                          std::thread firstEmitter([&] {
                              registry.emit();
                          });
                          firstEntered.wait();

                          std::promise<void> removeDone;
                          auto               removeFuture = removeDone.get_future();
                          std::thread        remover([&] {
                              registry.remove(handle);
                              removeReturned.store(true, std::memory_order_release);
                              removeDone.set_value();
                          });

                          // A brief head start so remover has actually retired the slot and entered
                          // waitForQuiescence's blocking wait - not still merely starting up - before
                          // the release below. Short enough to leave the subsequent race close.
                          std::this_thread::sleep_for(std::chrono::microseconds{200});

                          firstRelease.count_down();
                          // Busy-poll activeCount() rather than joining firstEmitter or sleeping:
                          // both of those are voluntary yield points that tend to let the scheduler
                          // give the (already notified) remover thread a turn to re-observe the
                          // slot's state before this thread reuses it. A tight, non-yielding spin
                          // reacts to finalize() removing the slot from the published snapshot -
                          // which happens synchronously inside exit(), before emit() can return -
                          // about as fast as this thread can, racing to reuse the slot before the
                          // remover thread gets rescheduled.
                          while (registry.activeCount() != 0) {}

                          // Immediately hand the exact same, now-recycled slot to a second,
                          // currently busy registration. A remove() that only checks the slot's
                          // count instead of its generation would now be waiting on *this*
                          // callback instead of the one it was asked to remove.
                          std::latch  secondEntered{1};
                          std::latch  secondRelease{1};
                          const auto  secondHandle = registry.add([&] {
                              secondEntered.count_down();
                              secondRelease.wait();
                          });
                          std::thread secondEmitter([&] {
                              registry.emit();
                          });
                          secondEntered.wait();

                          // The second callback is deliberately still blocked here. remove() must
                          // have already returned (or be about to, independent of secondRelease) -
                          // not be waiting on it.
                          joinWithinBoundOrAbort(remover,
                                                 removeFuture,
                                                 std::chrono::seconds{5},
                                                 "remove() waiting on a callback that only reused its retired slot");
                          checks.expect(removeReturned.load(std::memory_order_acquire),
                                        "remove() returned without regard to the reused slot's unrelated in-flight callback");

                          secondRelease.count_down();
                          secondEmitter.join();
                          registry.remove(secondHandle);
                          firstEmitter.join();
                      }

                      stopNoise.store(true, std::memory_order_relaxed);
                      for (auto& noise : noiseThreads)
                          noise.join();

                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register selfRemovalIsDeferred{"SinkRegistry: A callback that removes its own handle returns immediately and is never invoked again",
                                              "unit",
                                              [] {
                                                  return speclab::Test("sink-registry-self-removal-is-deferred")
                                                      .Then("the self-removing invocation completes and no further invocation starts",
                                                            [] {
                                                                speclab::core::Checks    checks;
                                                                CountingRegistry         registry;
                                                                int                      calls = 0;
                                                                CountingRegistry::Handle handle;

                                                                handle = registry.add([&] {
                                                                    ++calls;
                                                                    registry.remove(handle);  // self-removal: must return immediately, not deadlock
                                                                });

                                                                registry.emit();
                                                                checks.expect(calls == 1, "the self-removing callback ran exactly once");
                                                                checks.expect(registry.activeCount() == 0,
                                                                              "the handle is fully retired after the invocation returns");

                                                                registry.emit();
                                                                checks.expect(calls == 1, "no new invocation started after self-removal");
                                                                checks.raise();
                                                            })
                                                      .Execute();
                                              }};

const speclab::Register crossRemovalOfIdleSink{
    "SinkRegistry: A callback removing a different, currently-idle callback waits (trivially) and fully removes it",
    "unit",
    [] {
        return speclab::Test("sink-registry-cross-removal-of-idle-sink")
            .Then("the target stops receiving emissions and the remover completes normally",
                  [] {
                      speclab::core::Checks    checks;
                      CountingRegistry         registry;
                      int                      aCalls = 0;
                      int                      bCalls = 0;
                      CountingRegistry::Handle handleB;

                      handleB            = registry.add([&] {
                          ++bCalls;
                      });
                      const auto handleA = registry.add([&] {
                          ++aCalls;
                          registry.remove(handleB);
                      });

                      registry.emit();
                      checks.expect(aCalls == 1, "A ran");
                      checks.expect(bCalls == 1, "B ran once before being removed by A in this same pass, or was already retired - either is valid");
                      checks.expect(registry.activeCount() == 1, "only A remains registered");

                      registry.emit();
                      checks.expect(aCalls == 2, "A still runs");
                      checks.expect(bCalls == 1, "B never runs again once removed");
                      checks.raise();
                  })
            .Execute();
    }};

enum class WhichSink : std::uint8_t { A, B };
using DiscriminatedRegistry = SinkRegistry<void(WhichSink)>;

const speclab::Register mutualCrossRemovalAvoidsDeadlock{
    "SinkRegistry: Two callbacks concurrently removing each other from inside their own invocations do not deadlock",
    "unit",
    [] {
        return speclab::Test("sink-registry-mutual-cross-removal-avoids-deadlock")
            .Then("both sides complete within a bounded time instead of waiting on each other forever",
                  [] {
                      speclab::core::Checks         checks;
                      DiscriminatedRegistry         registry;
                      std::latch                    bothEntered{2};
                      DiscriminatedRegistry::Handle handleA;
                      DiscriminatedRegistry::Handle handleB;

                      handleA = registry.add([&](WhichSink which) {
                          if (which != WhichSink::A)
                              return;
                          bothEntered.count_down();
                          bothEntered.wait();
                          registry.remove(handleB);
                      });
                      handleB = registry.add([&](WhichSink which) {
                          if (which != WhichSink::B)
                              return;
                          bothEntered.count_down();
                          bothEntered.wait();
                          registry.remove(handleA);
                      });

                      std::promise<void> doneA;
                      std::promise<void> doneB;
                      auto               futureA = doneA.get_future();
                      auto               futureB = doneB.get_future();

                      std::thread threadA([&] {
                          registry.emit(WhichSink::A);
                          doneA.set_value();
                      });
                      std::thread threadB([&] {
                          registry.emit(WhichSink::B);
                          doneB.set_value();
                      });

                      // A real deadlock here must fail this test process outright, not hang CI - see
                      // joinWithinBoundOrAbort().
                      constexpr auto bound = std::chrono::seconds{5};
                      joinWithinBoundOrAbort(threadA, futureA, bound, "thread A's mutual-cross-removal emit()");
                      joinWithinBoundOrAbort(threadB, futureB, bound, "thread B's mutual-cross-removal emit()");

                      checks.expect(true, "mutual cross-removal completed within the time bound instead of hanging or aborting");
                      checks.expect(registry.activeCount() == 0, "both handles finish fully retired");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register destroyedOwnerNotAccessedAfterRemoval{
    "SinkRegistry: A callback's captured owner is never touched after remove() returns, even under concurrent emission",
    "unit",
    [] {
        return speclab::Test("sink-registry-no-access-after-owner-destruction")
            .Then("destroying the owner right after remove() returns is safe under a concurrent emitter",
                  [] {
                      speclab::core::Checks checks;

                      struct Owner {
                          std::atomic<int> calls{0};
                          void             onEvent() {
                              calls.fetch_add(1, std::memory_order_relaxed);
                          }
                      };

                      CountingRegistry registry;
                      auto             owner  = std::make_unique<Owner>();
                      Owner*           raw    = owner.get();
                      const auto       handle = registry.add([raw] {
                          raw->onEvent();
                      });

                      std::atomic<bool> stop{false};
                      std::thread       emitter([&] {
                          while (!stop.load(std::memory_order_acquire))
                              registry.emit();
                      });

                      // Give the emitter a chance to actually race with the removal below.
                      std::this_thread::sleep_for(std::chrono::milliseconds{20});

                      registry.remove(handle);  // must fully wait before returning
                      owner.reset();            // if remove() did not truly wait, this is a use-after-free

                      std::this_thread::sleep_for(std::chrono::milliseconds{20});
                      stop.store(true, std::memory_order_release);
                      emitter.join();

                      checks.expect(true, "no crash and no sanitizer report: the owner was not reachable after remove() returned");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register slotsAreRecycled{"SinkRegistry: Repeated add()/remove() cycles recycle slots instead of growing the registry without bound",
                                         "unit",
                                         [] {
                                             return speclab::Test("sink-registry-slots-are-recycled")
                                                 .Then("the pool of distinct slot allocations stays small across many churn cycles",
                                                       [] {
                                                           speclab::core::Checks checks;
                                                           CountingRegistry      registry;

                                                           constexpr int kCycles = 500;
                                                           for (int i = 0; i < kCycles; ++i) {
                                                               const auto handle = registry.add([] {});
                                                               registry.remove(handle);
                                                           }

                                                           checks.expect(registry.activeCount() == 0, "nothing remains registered");
                                                           checks.expect(registry.poolSize() < static_cast<std::size_t>(kCycles),
                                                                         "slot allocations are recycled through the free list, not one per cycle");
                                                           checks.raise();
                                                       })
                                                 .Execute();
                                         }};

const speclab::Register nestedEmissionAncestorRemovalDoesNotDeadlock{
    "SinkRegistry: removing an ancestor frame's handle from a nested emit() call does not deadlock",
    "unit",
    [] {
        return speclab::Test("sink-registry-nested-emission-ancestor-removal")
            .Then("self-removal is recognized against every frame on the calling thread's stack, not only the "
                  "innermost one - otherwise the outer frame would wait on its own quiescence forever",
                  [] {
                      speclab::core::Checks    checks;
                      CountingRegistry         registry;
                      int                      aCalls = 0;
                      int                      bCalls = 0;
                      CountingRegistry::Handle handleA;

                      // A's outermost invocation calls emit() again, nesting B's invocation inside it while
                      // A is still on this thread's invocation stack. From there, B removes A - an ancestor
                      // frame, not B's own - which the old innermost-only self-check treated as an ordinary
                      // cross-removal and blocked waiting for A's quiescence, which only A's own (blocked)
                      // thread could ever provide.
                      handleA                             = registry.add([&] {
                          ++aCalls;
                          if (aCalls == 1)
                              registry.emit();
                      });
                      [[maybe_unused]] const auto handleB = registry.add([&] {
                          ++bCalls;
                          registry.remove(handleA);
                      });

                      std::promise<void> done;
                      auto               future = done.get_future();
                      std::thread        worker([&] {
                          registry.emit();
                          done.set_value();
                      });

                      // A real regression here must fail this test process outright, not hang CI - see
                      // joinWithinBoundOrAbort().
                      joinWithinBoundOrAbort(worker, future, std::chrono::seconds{5}, "the nested-emission worker");

                      checks.expect(true, "the nested emission completed instead of the outer frame deadlocking on itself");
                      checks.expect(aCalls >= 1 && bCalls >= 1, "both callbacks ran");
                      checks.expect(registry.activeCount() == 1, "only B remains registered; A was removed from the nested callback");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register handleFromAnotherRegistryIsRejected{
    "SinkRegistry: remove() ignores a handle issued by a different registry instance",
    "unit",
    [] {
        return speclab::Test("sink-registry-cross-registry-handle-rejected")
            .Then("the foreign call is a no-op and does not corrupt either registry's bookkeeping",
                  [] {
                      speclab::core::Checks checks;
                      CountingRegistry      registryA;
                      CountingRegistry      registryB;
                      int                   calls = 0;

                      const auto handleFromA = registryA.add([&] {
                          ++calls;
                      });

                      // handleFromA was issued by registryA. Handing it to registryB's remove() must not
                      // touch registryA's slot (clearing its callback, advancing its generation) or leave
                      // registryA's own snapshot holding a now-stale entry.
                      registryB.remove(handleFromA);

                      checks.expect(registryA.activeCount() == 1, "registryA still reports its callback as registered");

                      registryA.emit();
                      checks.expect(calls == 1, "the callback registryB was wrongly handed still runs normally through its own registry");

                      registryA.remove(handleFromA);
                      checks.expect(registryA.activeCount() == 0, "removing through the OWNING registry still works");
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
