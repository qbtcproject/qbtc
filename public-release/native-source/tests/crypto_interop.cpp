#include "qbtc/wallet.hpp"
#include <iostream>
using namespace qbtc;
int main(int argc, char **argv) {
    try {
        if (argc != 3)
            throw Invalid("interop usage: emit|verify DIRECTORY");
        auto path = std::string(argv[2]);
        if (std::string(argv[1]) == "emit") {
            auto key = wallet::Key::generate();
            Bytes message{'Q', 'B', 'T', 'C', '/', 'i', 'n', 't', 'e', 'r', 'o', 'p', '/', '1'};
            wallet::write_private_new(path + "/cpp-public.bin", key.public_key());
            wallet::write_private_new(path + "/cpp-message.bin", message);
            wallet::write_private_new(path + "/cpp-signature.bin", key.sign(message));
            return 0;
        }
        if (std::string(argv[1]) == "verify") {
            auto pk = wallet::read_bounded(path + "/python-public.bin", 4096),
                 msg = wallet::read_bounded(path + "/python-message.bin", MAX_BLOCK_BYTES),
                 sig = wallet::read_bounded(path + "/python-signature.bin", 16384);
            if (!verify_signature(pk, msg, sig))
                throw Invalid("Python-to-C++ signature failed");
            std::cout << "PASS Python/OpenSSL-to-C++/OpenSSL ML-DSA verification\n";
            return 0;
        }
        throw Invalid("unknown interop command");
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
