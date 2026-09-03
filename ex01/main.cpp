#include <iostream>

// TIP To <b>Run</b> code, press <shortcut actionId="Run"/> or click the <icon src="AllIcons.Actions.Execute"/> icon in the gutter.
typedef struct Person {
    int pid;
    char name[10];
    int age;
    char city[2];
} Person;

typedef struct Person2 {
    int pid;
    int age;
    char name[10];
    char city[2];
} Person2;

typedef struct Person3 {
    int pid;
    int age;
    char name[10];
    char city[2];
    bool veteran;
} Person3;
int main() {
    // TIP Press <shortcut actionId="RenameElement"/> when your caret is at the <b>lang</b> variable name to see how CLion can help you rename it.
    Person p1 = {
        1,
        "Joe",
        25,
        {'N', 'Y'}
    };
    int len = sizeof(p1);
    std::cout << "len p1: " << len<< std::endl;
    len  = sizeof(p1.pid);
    std::cout << "len p1.pid: " << len<< std::endl;

    len  = sizeof(p1.name);
    std::cout << "len p1.name: " << len << std::endl;

    len  = sizeof(p1.age);
    std::cout << "len p1.age: " << len << std::endl;

    len  = sizeof(p1.city);
    std::cout << "len p1.city: " << len << std::endl;


    Person2 p2 = {
        1,
        25,
        "Joe",
        {'N', 'Y'}
    };
    int len2 = sizeof(p2);
    std::cout << "len p2: " << len2<< std::endl;


    Person3 p3 = {
        1,
        25,
        "Joe",
        {'N', 'Y'},
        true
    };
    int len3 = sizeof(p3);
    std::cout << "len p3: " << len3<< std::endl;
    return 0;
    // TIP See CLion help at <a href="https://www.jetbrains.com/help/clion/">jetbrains.com/help/clion/</a>. Also, you can try interactive lessons for CLion by selecting 'Help | Learn IDE Features' from the main menu.
}