 
#ifndef ASCII_H
#define ASCII_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

/*
    +-----+-----+----------+-------+---------------------------+
    | DEC | HEX | BIN      | Glyph | Description               |
    +-----+-----+----------+-------+---------------------------+
    | 0   | 0   | 00000000 | NUL   | Null                      |
    | 1   | 1   | 00000001 | SOH   | Start of Header           |
    | 2   | 2   | 00000010 | STX   | Start of Text             |
    | 3   | 3   | 00000011 | ETX   | End of Text               |
    | 4   | 4   | 00000100 | EOT   | End of Transmission       |
    | 5   | 5   | 00000101 | ENQ   | Enquiry                   |
    | 6   | 6   | 00000110 | ACK   | Acknowledge               |
    | 7   | 7   | 00000111 | BEL   | Bell                      |
    | 8   | 8   | 00001000 | BS    | Backspace                 |
    | 9   | 9   | 00001001 | HT    | Horizontal Tab            |
    | 10  | a   | 00001010 | LF    | Line Feed                 |
    | 11  | b   | 00001011 | VT    | Vertical Tab              |
    | 12  | c   | 00001100 | FF    | Form Feed                 |
    | 13  | d   | 00001101 | CR    | Carriage Return           |
    | 14  | e   | 00001110 | SO    | Shift Out                 |
    | 15  | f   | 00001111 | SI    | Shift In                  |
    | 16  | 10  | 00010000 | DLE   | Data Link Escape          |
    | 17  | 11  | 00010001 | DC1   | Device Control 1          |
    | 18  | 12  | 00010010 | DC2   | Device Control 2          |
    | 19  | 13  | 00010011 | DC3   | Device Control 3          |
    | 20  | 14  | 00010100 | DC4   | Device Control 4          |
    | 21  | 15  | 00010101 | NAK   | Negative Acknowledge      |
    | 22  | 16  | 00010110 | SYN   | Synchronize               |
    | 23  | 17  | 00010111 | ETB   | End of Transmission Block |
    | 24  | 18  | 00011000 | CAN   | Cancel                    |
    | 25  | 19  | 00011001 | EM    | End of Medium             |
    | 26  | 1a  | 00011010 | SUB   | Substitute                |
    | 27  | 1b  | 00011011 | ESC   | Escape                    |
    | 28  | 1c  | 00011100 | FS    | File Separator            |
    | 29  | 1d  | 00011101 | GS    | Group Separator           |
    | 30  | 1e  | 00011110 | RS    | Record Separator          |
    | 31  | 1f  | 00011111 | US    | Unit Separator            |
    | 32  | 20  | 00100000 | space | Space                     |
    | 33  | 21  | 00100001 | !     | Exclamation mark          |
    | 34  | 22  | 00100010 | "     | Double quote              |
    | 35  | 23  | 00100011 | #     | Number                    |
    | 36  | 24  | 00100100 | $     | Dollar sign               |
    | 37  | 25  | 00100101 | %     | Percent                   |
    | 38  | 26  | 00100110 | &     | Ampersand                 |
    | 39  | 27  | 00100111 | '     | Single quote              |
    | 40  | 28  | 00101000 | (     | Left parenthesis          |
    | 41  | 29  | 00101001 | )     | Right parenthesis         |
    | 42  | 2a  | 00101010 | *     | Asterisk                  |
    | 43  | 2b  | 00101011 | +     | Plus                      |
    | 44  | 2c  | 00101100 | ,     | Comma                     |
    | 45  | 2d  | 00101101 | -     | Minus                     |
    | 46  | 2e  | 00101110 | .     | Period                    |
    | 47  | 2f  | 00101111 | /     | Slash                     |
    | 48  | 30  | 00110000 | 0     | Zero                      |
    | 49  | 31  | 00110001 | 1     | One                       |
    | 50  | 32  | 00110010 | 2     | Two                       |
    | 51  | 33  | 00110011 | 3     | Three                     |
    | 52  | 34  | 00110100 | 4     | Four                      |
    | 53  | 35  | 00110101 | 5     | Five                      |
    | 54  | 36  | 00110110 | 6     | Six                       |
    | 55  | 37  | 00110111 | 7     | Seven                     |
    | 56  | 38  | 00111000 | 8     | Eight                     |
    | 57  | 39  | 00111001 | 9     | Nine                      |
    | 58  | 3a  | 00111010 | :     | Colon                     |
    | 59  | 3b  | 00111011 | ;     | Semicolon                 |
    | 60  | 3c  | 00111100 | <     | Less than                 |
    | 61  | 3d  | 00111101 | =     | Equality sign             |
    | 62  | 3e  | 00111110 | >     | Greater than              |
    | 63  | 3f  | 00111111 | ?     | Question mark             |
    | 64  | 40  | 01000000 | @     | At sign                   |
    | 65  | 41  | 01000001 | A     | Capital A                 |
    | 66  | 42  | 01000010 | B     | Capital B                 |
    | 67  | 43  | 01000011 | C     | Capital C                 |
    | 68  | 44  | 01000100 | D     | Capital D                 |
    | 69  | 45  | 01000101 | E     | Capital E                 |
    | 70  | 46  | 01000110 | F     | Capital F                 |
    | 71  | 47  | 01000111 | G     | Capital G                 |
    | 72  | 48  | 01001000 | H     | Capital H                 |
    | 73  | 49  | 01001001 | I     | Capital I                 |
    | 74  | 4a  | 01001010 | J     | Capital J                 |
    | 75  | 4b  | 01001011 | K     | Capital K                 |
    | 76  | 4c  | 01001100 | L     | Capital L                 |
    | 77  | 4d  | 01001101 | M     | Capital M                 |
    | 78  | 4e  | 01001110 | N     | Capital N                 |
    | 79  | 4f  | 01001111 | O     | Capital O                 |
    | 80  | 50  | 01010000 | P     | Capital P                 |
    | 81  | 51  | 01010001 | Q     | Capital Q                 |
    | 82  | 52  | 01010010 | R     | Capital R                 |
    | 83  | 53  | 01010011 | S     | Capital S                 |
    | 84  | 54  | 01010100 | T     | Capital T                 |
    | 85  | 55  | 01010101 | U     | Capital U                 |
    | 86  | 56  | 01010110 | V     | Capital V                 |
    | 87  | 57  | 01010111 | W     | Capital W                 |
    | 88  | 58  | 01011000 | X     | Capital X                 |
    | 89  | 59  | 01011001 | Y     | Capital Y                 |
    | 90  | 5a  | 01011010 | Z     | Capital Z                 |
    | 91  | 5b  | 01011011 | [     | Left square bracket       |
    | 92  | 5c  | 01011100 | \     | Backslash                 |
    | 93  | 5d  | 01011101 | ]     | Right square bracket      |
    | 94  | 5e  | 01011110 | ^     | Caret / circumflex        |
    | 95  | 5f  | 01011111 | _     | Underscore                |
    | 96  | 60  | 01100000 | `     | Grave / accent            |
    | 97  | 61  | 01100001 | a     | Small a                   |
    | 98  | 62  | 01100010 | b     | Small b                   |
    | 99  | 63  | 01100011 | c     | Small c                   |
    | 100 | 64  | 01100100 | d     | Small d                   |
    | 101 | 65  | 01100101 | e     | Small e                   |
    | 102 | 66  | 01100110 | f     | Small f                   |
    | 103 | 67  | 01100111 | g     | Small g                   |
    | 104 | 68  | 01101000 | h     | Small h                   |
    | 105 | 69  | 01101001 | i     | Small i                   |
    | 106 | 6a  | 01101010 | j     | Small j                   |
    | 107 | 6b  | 01101011 | k     | Small k                   |
    | 108 | 6c  | 01101100 | l     | Small l                   |
    | 109 | 6d  | 01101101 | m     | Small m                   |
    | 110 | 6e  | 01101110 | n     | Small n                   |
    | 111 | 6f  | 01101111 | o     | Small o                   |
    | 112 | 70  | 01110000 | p     | Small p                   |
    | 113 | 71  | 01110001 | q     | Small q                   |
    | 114 | 72  | 01110010 | r     | Small r                   |
    | 115 | 73  | 01110011 | s     | Small s                   |
    | 116 | 74  | 01110100 | t     | Small t                   |
    | 117 | 75  | 01110101 | u     | Small u                   |
    | 118 | 76  | 01110110 | v     | Small v                   |
    | 119 | 77  | 01110111 | w     | Small w                   |
    | 120 | 78  | 01111000 | x     | Small x                   |
    | 121 | 79  | 01111001 | y     | Small y                   |
    | 122 | 7a  | 01111010 | z     | Small z                   |
    | 123 | 7b  | 01111011 | {     | Left curly bracket        |
    | 124 | 7c  | 01111100 | |     | Vertical bar              |
    | 125 | 7d  | 01111101 | }     | Right curly bracket       |
    | 126 | 7e  | 01111110 | ~     | Tilde                     |
    | 127 | 7f  | 01111111 | DEL   | Delete                    |
    +-----+-----+----------+-------+---------------------------+
*/

#include "common/predef.h" /* nodiscard, inline, static_cast */

typedef unsigned char ascii;

nodiscard static inline bool ascii_is_alphanumeric(ascii c);
nodiscard static inline bool ascii_is_alphabetic(ascii c);
nodiscard static inline bool ascii_is_ascii(ascii c);
nodiscard static inline bool ascii_is_control(ascii c);
nodiscard static inline bool ascii_is_digit(ascii c);
nodiscard static inline bool ascii_is_hex(ascii c);
nodiscard static inline bool ascii_is_lower(ascii c);
nodiscard static inline bool ascii_is_print(ascii c);
nodiscard static inline bool ascii_is_punctuation(ascii c);
nodiscard static inline bool ascii_is_upper(ascii c);
nodiscard static inline bool ascii_is_whitespace(ascii c);
nodiscard static inline char ascii_to_lower(ascii c);
nodiscard static inline char ascii_to_upper(ascii c);

// [A-Za-z0-9]
nodiscard static inline bool ascii_is_alphanumeric(ascii c)
{
    return ascii_is_alphabetic(c) || ascii_is_digit(c);
}

// [A-Za-z]
nodiscard static inline bool ascii_is_alphabetic(ascii c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// [0-9A-Fa-f]
nodiscard static inline bool ascii_is_ascii(ascii c)
{
    return c < 128;
}

// any character before space, plus DEL
nodiscard static inline bool ascii_is_control(ascii c)
{
    return c < ' ' || c == 0x7f;
}

// [0-9]
nodiscard static inline bool ascii_is_digit(ascii c)
{
    return c >= '0' && c <= '9';
}

// [0-9A-Fa-f]
nodiscard static inline bool ascii_is_hex(ascii c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// [a-z]
nodiscard static inline bool ascii_is_lower(ascii c)
{
	return c >= 'a' && c <= 'z';
}

// [A-Za-z0-9] or punctuation or space
nodiscard static inline bool ascii_is_print(ascii c)
{
    return c >= ' ' && c <= '~';
}

// punctuation
nodiscard static inline bool ascii_is_punctuation(ascii c)
{
    return ascii_is_print(c) && !ascii_is_alphanumeric(c);
}

// [A-Z]
nodiscard static inline bool ascii_is_upper(ascii c)
{
	return c >= 'A' && c <= 'Z';
}

// [ \f\n\r\t\v]
nodiscard static inline bool ascii_is_whitespace(ascii c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

nodiscard static inline char ascii_to_lower(ascii c)
{
    if (ascii_is_upper(c)) c |= 0b00100000;
    return c;
}

nodiscard static inline char ascii_to_upper(ascii c)
{
    if (ascii_is_lower(c)) c &= 0b11011111;
    return c;
}

#endif /* ASCII_H */