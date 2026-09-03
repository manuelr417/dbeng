#include <iostream>
#include <string>
#include <cassert>
#include "Person.h"

// TIP To <b>Run</b> code, press <shortcut actionId="Run"/> or click the <icon src="AllIcons.Actions.Execute"/> icon in the gutter.
void print_person(std::string var_name, Person person);
void copy_person_to_buffer(Person person, int person_len, char* buffer, int max_len);
void copy_person_from_buffer(char* buffer, int max_len, Person *person);

int main() {
    // TIP Press <shortcut actionId="RenameElement"/> when your caret is at the <b>lang</b> variable name to see how CLion can help you rename it.

    const int max_len = 20;
    char *buffer = new char[max_len];
    const int person_len = 20;
    Person person1 = {
        1,
        "Joe",
        25,
        {'N', 'Y'}
    };
    // print person structure
    print_person("person1", person1);

    // create a second one
    Person person2 = {};
    //print empty person
    print_person("person2", person2);

    // now copy person1 to a buffer
    copy_person_to_buffer(person1, person_len, buffer, max_len);
    // now change a value in person 1

    person1.age = 50;
    // now copy person 2 from the buffer. this will be tje original value of person1
    copy_person_from_buffer(buffer, max_len, &person2);
    //print updated person 1
    print_person("person1", person1);
    // print updated person2
    print_person("person2", person2);
    delete[] buffer;
    return 0;
}
void print_person(std::string var_name, Person person) {
    std::cout << var_name << std::endl;
    std::cout << "{" << person.pid << ", ";
    std::cout << person.name << ", ";
    std::cout << person.age << ", ";
    std::cout << person.city << "}" << std::endl;
}
void copy_person_to_buffer(Person person, int person_len, char* buffer, int max_len) {
    assert(max_len >= person_len);
    int offset = 0;
    memcpy(buffer + offset, &person.pid, sizeof(person.pid));
    offset += sizeof(person.pid);
    memcpy(buffer + offset, person.name, sizeof(person.name));
    offset += sizeof(person.name);
    memcpy(buffer + offset, &person.age, sizeof(person.age));
    offset += sizeof(person.age);
    memcpy(buffer + offset, person.city, sizeof(person.city));

}
void copy_person_from_buffer(char* buffer, int max_len, Person *person) {
    int offset = 0;
    memcpy(&(person->pid), buffer + offset, sizeof(person->pid));
    offset += sizeof(person->pid);
    memcpy(person->name, buffer + offset, sizeof(person->name));
    offset += sizeof(person->name);
    memcpy(&(person->age), buffer + offset, sizeof(person->age));
    offset += sizeof(person->age);
    memcpy(person->city, buffer + offset, sizeof(person->city));
}
