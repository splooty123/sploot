#include <dirent.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    FILE* lib_file = fopen("src/libs.h", "w");
    if (!lib_file) {
        perror("fopen src/libs.h");
        return 1;
    }

    fprintf(lib_file, "\n#include \"compile.h\"\n");

    DIR* lib = opendir("lib");
    if (!lib) {
        perror("opendir lib");
        fclose(lib_file);
        return 1;
    }

    struct dirent* entry;
    while ((entry = readdir(lib)) != NULL) {
        // readdir() yields "." and ".." too; Path.iterdir() does not, so skip them.
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        fprintf(lib_file, "#include \"../lib/%s\"\n", entry->d_name);
    }

    closedir(lib);
    fclose(lib_file);
    return 0;
}