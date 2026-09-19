#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    std::string port = (argc > 1) ? argv[1] : "8080";
    std::printf("http-calc server stub, would listen on port %s\n", port.c_str());
    return 0;
}
