#include <iostream>

// TIP To <b>Run</b> code, press <shortcut actionId="Run"/> or click the <icon src="AllIcons.Actions.Execute"/> icon in the gutter.

int main() {
    // TIP Press <shortcut actionId="RenameElement"/> when your caret is at the <b>lang</b> variable name to see how CLion can help you rename it.

    int *p1 = new int;
    *p1 = 42;
    std::cout << "*p1: " << *p1 << std::endl;
    std::cout << "p1: " << p1 << std::endl;


    int n2 = 100;
    int *p2 = &n2;
    std::cout << "n2: " << n2 << std::endl;
    std::cout << "*p2: " << *p2 << std::endl;
    std::cout << "p2: " << p2 << std::endl;

    char *str = "Manuel\0";


    std::cout << "str: " << str << std::endl;
    std::cout << "str[0]: " << str[0] << std::endl;


    // pointer arithmetic
    char *str2 = str + 2;
    std::cout << "str2: " << str2 << std::endl;
    std::cout << "str2[0]: " << str2[0] << std::endl;


    char str3[10] = {'J', 'o', 'e'};

    std::cout << "str3: " << str3 << std::endl;
    std::cout << "str3[0]: " << str3[0] << std::endl;
    char *str4 = str3 + 1;
    std::cout << "str4: " << str4 << std::endl;
    std::cout << "str4[0]: " << str4[0] << std::endl;

    delete p1;
    return 0;
    // TIP See CLion help at <a href="https://www.jetbrains.com/help/clion/">jetbrains.com/help/clion/</a>. Also, you can try interactive lessons for CLion by selecting 'Help | Learn IDE Features' from the main menu.
}