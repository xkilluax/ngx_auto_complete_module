#include <stdio.h>
#include <string.h>

int main()
{
    char *s = "test汉子hello思密达.안녕하세요.こんにちは.א.world.𒍐cccc.卿.𧁓";
    char *last = s + strlen(s) - 1;
    char *p = s;
    char ub[7];

    while (*p) {
        if (*p & 0x80) {
            if ((*p & 0xfe) == 0xfc && last - p >= 5) {
                if ((*(p + 1) & 0xc0) == 0x80 && (*(p + 2) & 0xc0) == 0x80 && (*(p + 3) & 0xc0) == 0x80 && (*(p + 4) & 0xc0) == 0x80 && (*(p + 5) & 0xc0) == 0x80) {
                    memcpy(ub, p, 6);
                    ub[6] = '\0';
                    printf("Unicode(6) %s\n", ub);
                    p += 6;
                    continue;
                }
            } else if ((*p & 0xfc) == 0xf8 && last - p >= 4) {
                if ((*(p + 1) & 0xc0) == 0x80 && (*(p + 2) & 0xc0) == 0x80 && (*(p + 3) & 0xc0) == 0x80 && (*(p + 4) & 0xc0) == 0x80) {
                    memcpy(ub, p, 5);
                    ub[5] = '\0';
                    printf("Unicode(5) %s\n", ub);
                    p += 5;
                    continue;
                }
            } if ((*p & 0xf8) == 0xf0 && last - p >= 3) {
                if ((*(p + 1) & 0xc0) == 0x80 && (*(p + 2) & 0xc0) == 0x80 && (*(p + 3) & 0xc0) == 0x80) {
                    memcpy(ub, p, 4);
                    ub[4] = '\0';
                    printf("Unicode(4) %s\n", ub);
                    p += 4;
                    continue;
                }
            } else if ((*p & 0xf0) == 0xe0 && last - p >= 2) {
                if ((*(p + 1) & 0xc0) == 0x80 && (*(p + 2) & 0xc0) == 0x80) {
                    memcpy(ub, p, 3);
                    ub[3] = '\0';
                    printf("Unicode(3) %s\n", ub);
                    p += 3;
                    continue;

                }
            } else if ((*p & 0xe0) == 0xc0 && last - p >= 1) {
                if ((*(p + 1) & 0xc0) == 0x80) {
                    memcpy(ub, p, 2);
                    ub[2] = '\0';
                    printf("Unicode(2) %s\n", ub);
                    p += 2;
                    continue;
                }
            } else {
                printf("Unknown %c", *p);
                p++;
                continue;
            }
        } else {
            printf("ANSCII %c\n", *p);
            p++;
        }


        /*if (*p) {
            if (*p & 0x80) {
                printf("%s\n", p);
                p += 3;
            } else {
                printf("%c\n", *p);
                p++;
            }
        }*/
    }

    return 0;
}
