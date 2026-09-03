#include <cstring>
#include <iostream>
#include <memory>

// TIP To <b>Run</b> code, press <shortcut actionId="Run"/> or click the <icon src="AllIcons.Actions.Execute"/> icon in the gutter.

int main() {

    const int val1 = 40;
    int val2 = 0;
    const int buf_len = 16;
    std::unique_ptr<char[]> buffer = std::make_unique<char[]>(buf_len);

    // show values
    std::cout << "val1: " << val1 << std::endl;
    std::cout << "val2: " << val2 << std::endl;

    // do work
    std::cout << "Making copy to buffer" << std::endl;
    memcpy(buffer.get(), &val1, sizeof(int));
    std::cout << "Making copy to val2: " << std::endl;
    memcpy(&val2, buffer.get(), sizeof(int));

    std::cout << "Values after copy" << std::endl;
    // show values
    std::cout << "val1: " << val1 << std::endl;
    std::cout << "val2: " << val2 << std::endl;

    return 0;

}
