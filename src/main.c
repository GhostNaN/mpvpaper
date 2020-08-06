/*
 *  Copyright (C) 2019-2020 Scoopta
 *  This file is part of GLPaper
 *  GLPaper is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    GLPaper is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with GLPaper.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <getopt.h>
#include <string.h>
#include <stdlib.h>

#include <paper.h>

static void print_usage(char** argv) {
    char* slash = strrchr(argv[0], '/');
    uint64_t offset;
    if(slash == NULL) {
        offset = 0;
    } else {
        offset = (slash - argv[0]) + 1;
    }
    printf("%s [options] <output> <url|path filename>\n", argv[0] + offset);
    printf("Options:\n");
    printf("--help\t\t-h\tDisplays this help message\n");
    printf("--verbose\t-v\tBe more verbose\n");
    printf("--fork\t\t-f\tForks mpvpaper so you can close the terminal\n");
    printf("--layer\t\t-l\tSpecifies shell layer to run on (background by default)\n");
    printf("--mpv-options\t-o\tForwards mpv options (Must be within quotes\"\")\n");
    exit(0);
}

int main(int argc, char** argv) {
    if(argc > 2) {
        struct option opts[] = {
            {
                .name = "help",
                .has_arg = no_argument,
                .flag = NULL,
                .val = 'h'
            },
            {
                .name = "verbose",
                .has_arg = no_argument,
                .flag = NULL,
                .val = 'v'
            },
            {
                .name = "fork",
                .has_arg = no_argument,
                .flag = NULL,
                .val = 'f'
            },
            {
                .name = "layer",
                .has_arg = required_argument,
                .flag = NULL,
                .val = 'l'
            },
            {
                .name = "mpv-options",
                .has_arg = required_argument,
                .flag = NULL,
                .val = 'o'
            },
            {
                .name = NULL,
                .has_arg = 0,
                .flag = NULL,
                .val = 0
            }
        };
        int verbose = 0;
        char* layer = NULL;
        char* mpv_options = NULL;
        char opt;
        while((opt = getopt_long(argc, argv, "hvfl:o:", opts, NULL)) != -1) {
            switch(opt) {
            case 'h':
                print_usage(argv);
                break;
            case 'v':
                verbose = 1;
                break;
            case 'f':
                if(fork() > 0) {
                    exit(0);
                }
                fclose(stdout);
                fclose(stderr);
                fclose(stdin);
                break;
            case 'l':
                layer = optarg;
                break;
            case 'o':
                mpv_options = optarg;
                // Forward to a tmp file so mpv can parse options
                for (int i = 0; i < (int)strlen(mpv_options); i++) {
                    if (mpv_options[i] == ' ') mpv_options[i] = '\n';
                }
                FILE* config = fopen("/tmp/mpvpaper.conf", "w");
                fputs(mpv_options, config);
                fclose(config);
                break;
            }
        }
        if(optind + 1 >= argc) {
            print_usage(argv);
        }

        paper_init(argv[optind], argv[optind + 1], verbose, layer);
    } else {
        print_usage(argv);
    }
}
