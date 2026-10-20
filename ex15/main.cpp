// ex15 — the check suite. Three layers of regression: the vendored
// ex11 storage suite, the vendored ex12 sql suite, then the exercise
// additions (index module, hash-index module, exec layer, bulk loader,
// group-by module, differential oracle) as they land. Exit code 0 iff
// every check passes.

#include <iostream>

int run_storage_checks();
int run_sql_checks();
int run_index_checks();
int run_hash_module_checks();
int run_static_hash_module_checks();
int run_exec_checks();
int run_loader_checks();
int run_group_checks();

int main() {
    const int storage_failures = run_storage_checks();
    const int sql_failures = run_sql_checks();
    const int index_failures = run_index_checks();
    const int hash_failures = run_hash_module_checks();
    const int static_failures = run_static_hash_module_checks();
    const int exec_failures = run_exec_checks();
    const int loader_failures = run_loader_checks();
    const int group_failures = run_group_checks();
    const int failures = storage_failures + sql_failures + index_failures +
                         hash_failures + static_failures + exec_failures +
                         loader_failures + group_failures;
    std::cout << (failures == 0 ? "\nAll checks passed." : "\nSome checks FAILED.")
              << "\n";
    return failures == 0 ? 0 : 1;
}
