/** @brief SHA-256 probe compared with Python hashlib, outside the implementation under test. */
import std;
import mddlog.adapter.auditchain;

// NOLINTNEXTLINE(bugprone-exception-escape): test-only command; failures abort instead of returning a digest.
int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.size() > 131072 || line.size() % 2 != 0)
            return 1;
        std::vector<std::uint8_t> bytes;
        bytes.reserve(line.size() / 2);
        constexpr std::string_view digits = "0123456789abcdef";
        for (std::size_t k = 0; k < line.size(); k += 2) {
            const auto high = digits.find(line.at(k));
            const auto low  = digits.find(line.at(k + 1));
            if (high == std::string_view::npos || low == std::string_view::npos)
                return 1;
            bytes.push_back(static_cast<std::uint8_t>((high * 16) + low));
        }
        const auto digest = mddlog::adapter::digestToHex(mddlog::adapter::sha256(bytes));
        std::cout << std::string_view{digest.data(), digest.size()} << '\n';
    }
    return std::cin.eof() ? 0 : 1;
}
