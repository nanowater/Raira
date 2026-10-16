#include "platform/http.h"

std::string url_escape(const std::string & s)
{
    static const char HEX[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for(unsigned char c : s) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                          c == '_' || c == '.' || c == '~';
        if(unreserved) {
            out += (char)c;
        } else {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 15];
        }
    }
    return out;
}
