#include "libs.h"
#include <stddef.h>
#include <time.h>

int main(int argc, char** argv){
    clock_t start_time = clock();
    char* input = NULL;
    char* output = NULL;
    bool expand = false;
    int expand_depth = -1;
    verbose = 0;
    for(int i = 0; i < argc; i++){
        if(strcmp(argv[i], "-i") == 0 && i + 1 < argc){
            input = argv[i + 1];
        } else if(strcmp(argv[i], "-o") == 0 && i + 1 < argc){
            output = argv[i + 1];
        } else if(strcmp(argv[i], "--verbose") == 0){
            verbose = true;
        } else if(strcmp(argv[i], "--expand") == 0 || strcmp(argv[i], "-e") == 0){
            expand = true;
            if (i + 1 < argc && is_integer(argv[i + 1], argv[i + 1] + strlen(argv[i + 1]))) {
                expand_depth = atoi(argv[i + 1]);
                i++;
            }
        }
    }

    if (!input || (!output && !expand)) {
        fprintf(stderr, "You forgot the input/output file, fix it.");
        return 1;
    }

    FILE* input_file = fopen(input, "r");
    if (!input_file) {
        fprintf(stderr, "Error: Could not open input file %s\n", input);
        return 1;
    }
    
    fseek(input_file, 0, SEEK_END);
    long size = ftell(input_file);
    if (size < 0) {
        fprintf(stderr, "Error determining file size\n");
        fclose(input_file);
        return 1;
    }

    size_t input_size = (size_t)size;
    rewind(input_file);
    char* src = (char*)malloc(input_size + 1);
    if (!src) {
        fprintf(stderr, "Error: Out of memory\n");
        fclose(input_file);
        return 1;
    }

    if (fread(src, 1, input_size, input_file) != input_size) {
        fprintf(stderr, "Error: Could not read input file %s\n", input);
        free(src);
        fclose(input_file);
        return 1;
    }

    src[input_size] = '\0';
    if (expand) {
        char* expanded = dump_expansion(src, expand_depth);

        if (output) {
            FILE* output_file = fopen(output, "w");
            if (!output_file) {
                fprintf(stderr, "Error: Could not open output file %s\n", output);
                free(expanded);
                free(src);
                fclose(input_file);
                return 1;
            }
            fprintf(output_file, "%s\n", expanded);
            fclose(output_file);
        } else {
            printf("%s\n", expanded);
        }

        free(expanded);
    } else {
        compile(src, output);
    }
    free(src);
    fclose(input_file);
    clock_t end_time = clock();
    double cpu_time_used = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    fprintf(stderr, "Compilation took %f seconds.\n", cpu_time_used);
    return 0;
}