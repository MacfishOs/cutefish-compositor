#ifndef BASE_I18N_H_
#define BASE_I18N_H_

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace cutefish::i18n {
inline std::string tr(const char *text) { return text; }
inline std::string tr(const std::string &text) { return text; }

inline const char *trNoop(const char *text) { return text; }

inline std::string trc(const char *context, const char *text) { return text; }

inline void setDomain(const char *) {}

inline std::string format(std::string_view pattern,
                          std::initializer_list<std::pair<std::string_view, std::string_view>> arguments)
{
    std::string result;
    result.reserve(pattern.size());
    size_t position = 0;
    while (position < pattern.size()) {
        const size_t open = pattern.find('{', position);
        if (open == std::string_view::npos)
            break;
        const size_t close = pattern.find('}', open + 1);
        if (close == std::string_view::npos)
            break;
        result.append(pattern, position, open - position);
        const std::string_view name = pattern.substr(open + 1, close - open - 1);
        const auto argument = std::find_if(arguments.begin(), arguments.end(),
                                           [name](const auto& entry) { return entry.first == name; });
        if (argument != arguments.end())
            result.append(argument->second);
        else
            result.append(pattern, open, close - open + 1);
        position = close + 1;
    }
    result.append(pattern, position);
    return result;
}

inline std::string format(std::string_view pattern, const std::map<std::string, std::string> &args)
{
    std::string result;
    result.reserve(pattern.size());
    size_t position = 0;
    while (position < pattern.size()) {
        const size_t open = pattern.find('{', position);
        if (open == std::string_view::npos)
            break;
        const size_t close = pattern.find('}', open + 1);
        if (close == std::string_view::npos)
            break;
        result.append(pattern, position, open - position);
        const std::string name(pattern.substr(open + 1, close - open - 1));
        auto it = args.find(name);
        if (it != args.end())
            result.append(it->second);
        else
            result.append(pattern, open, close - open + 1);
        position = close + 1;
    }
    result.append(pattern, position);
    return result;
}

template<typename IntType>
inline std::string trn(const char *singular, const char *plural, IntType n) {
    const char *pattern = (n == 1) ? singular : plural;
    const std::string nStr = std::to_string(n);
    return format(pattern, {{"n", nStr}});
}

}

#endif // BASE_I18N_H_
