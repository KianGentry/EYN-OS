#include "../include/types.h"

uint16 strlength(string ch)
{
        uint16 i = 0;           //Changed counter to 0
        while(ch[i++]);
        return i-1;               //Changed counter to i instead of i--
}

uint8 cmdLength(string ch) {
        uint8 i = 0;
        uint8 size = 0;
        uint8 realSize = strlength(ch);
        uint8 hasSpace = 0;

        string space;
        space[0] = ' ';

        for (i; i <= realSize; i++) {
                if (hasSpace == 0) {
                        if (ch[i] != space[0]) {
                                size++;
                        } else {
                                hasSpace = 1;
                        }
                }
        }
        
        if (hasSpace != 1) {
                size--;
        }

        size--;
        return size;
}

uint8 strEql(string ch1,string ch2)
{
        uint8 result = 1;
        uint8 size = strlength(ch1);
        if(size != strlength(ch2)) result =0;
        else
        {
        uint8 i = 0;
        for(i;i<=size;i++)
        {
                if(ch1[i] != ch2[i]) result = 0;
        }
        }
        return result;
}

uint8 cmdEql(string ch1, string ch2) {
        uint8 res = 1;
        uint8 size = cmdLength(ch1);

        if (size != cmdLength(ch2)) res = 0;
        else {
                uint8 i = 0;
                for (i; i <= size; i++) {
                        if (ch1[i] != ch2[i]) {res = 0;}
                }
        }

        return res;
}
