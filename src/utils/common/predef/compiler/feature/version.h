#ifndef COMPILER_VERSION_H
#define COMPILER_VERSION_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#define COMPILER_VERSION_ENCODE(major, minor, revision) (((major) * 1000000) + ((minor) * 1000) + (revision))
#define COMPILER_VERSION_DECODE_MAJOR(version) ((version) / 1000000)
#define COMPILER_VERSION_DECODE_MINOR(version) (((version) % 1000000) / 1000)
#define COMPILER_VERSION_DECODE_REVISION(version) ((version) % 1000)

#endif /* COMPILER_VERSION_H */