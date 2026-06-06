/*
 * pikture.h
 * Native PIKTURE format encoder and decoder library.
 * Version: 101
 * * Copyright 2026 Nexoniarz
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PIKTURE_H
#define PIKTURE_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t depth;
    uint8_t channels;
    uint8_t endianness;
    uint8_t* pixels;
} pikture_t;

#ifdef __cplusplus
extern "C" {
#endif

pikture_t* pikture_load(const char* filename);

int pikture_save(const char* filename, const pikture_t* img);

void pikture_free(pikture_t* img);

#ifdef __cplusplus
}
#endif

#endif
