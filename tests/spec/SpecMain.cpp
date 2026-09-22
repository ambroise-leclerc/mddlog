/**
 * @brief Entry point for the mddlog SpecLab specification suite.
 *
 * Every *Spec.cpp file registers its scenarios at namespace scope via speclab::Register; this
 * translation unit only implements the --list-tests / --run=<name> contract that
 * cmake/MddlogTestDiscovery.cmake relies on to register one CTest entry per scenario.
 */
import std;
import speclab;

int main(int argc, char** argv) {
    return speclab::runMain(argc, argv, "mddlog specs");
}
