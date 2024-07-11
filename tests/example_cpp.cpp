#define DBGLOG

// Current log level
const int log_level = 2;

const char* arch = "sparc";

void log(char* str) {}

int main() {
    log(arch);
}
