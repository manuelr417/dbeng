#include <iostream>
#include <random>
#include <string>
#include <cassert>
#include "Person.h"

// TIP To <b>Run</b> code, press <shortcut actionId="Run"/> or click the <icon src="AllIcons.Actions.Execute"/> icon in the gutter.
void print_person(std::string var_name, Person person);
void copy_person_to_buffer(Person person, int person_len, char* buffer, int max_len);
void copy_person_from_buffer(char* buffer, int max_len, Person *person);
void copy_persons_to_buffer(Person persons[], int person_count, int person_len,
                             char* buffer, int max_len);
void copy_persons_from_buffer(char* buffer, int max_len, Person persons[],
                               int person_count, int person_len);
void print_persons(Person persons[], int person_count);

int main() {

    const int max_len = 128;
    const int person_len = 20;
    const int person_count = 4;
    char *buffer = new char[max_len]{};
    Person persons[person_count]{};
    Person copied_persons[person_count]{};

    std::mt19937 generator(42);
    std::uniform_int_distribution<int> pid_distribution(100, 999);
    std::uniform_int_distribution<int> age_distribution(1, 100);
    std::uniform_int_distribution<int> letter_distribution('A', 'Z');

    for (Person& person : persons) {
        person.pid = pid_distribution(generator);
        person.age = age_distribution(generator);
        for (int i = 0; i < 9; ++i) {
            person.name[i] = static_cast<char>(letter_distribution(generator));
        }
        person.name[9] = '\0';
        person.city[0] = static_cast<char>(letter_distribution(generator));
        person.city[1] = '\0';
    }
    std::cout << "Print Persons: " << std::endl;
    print_persons(persons, person_count);
    std::cout << "Print Copied Persons: " << std::endl;
    print_persons(copied_persons, person_count);

    std::cout << "Copy Persons: " << std::endl;
    copy_persons_to_buffer(persons, person_count, person_len, buffer, max_len);
    copy_persons_from_buffer(buffer, max_len, copied_persons, person_count, person_len);

    std::cout << "Print Copied Persons: " << std::endl;
    print_persons(copied_persons, person_count);

    delete[] buffer;
    return 0;
}

void copy_persons_to_buffer(Person persons[], int person_count, int person_len,
                            char* buffer, int max_len) {
    assert(max_len >= person_count * person_len);
    int offset = 0;
    for (int i = 0; i < person_count; ++i) {
        copy_person_to_buffer(persons[i], person_len, buffer + offset, max_len - offset);
        offset += person_len;
    }
}

void copy_persons_from_buffer(char* buffer, int max_len, Person persons[],
                              int person_count, int person_len) {
    assert(max_len >= person_count * person_len);
    int offset = 0;
    for (int i = 0; i < person_count; ++i) {
        copy_person_from_buffer(buffer + offset, max_len - offset, &persons[i]);
        offset += person_len;
    }
}

void print_persons(Person persons[], int person_count) {
    for (int i = 0; i < person_count; ++i) {
        print_person("person" + std::to_string(i + 1), persons[i]);
    }
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
