#ifndef BASE_I18N_H_
#define BASE_I18N_H_

#include <string>
#include <map>

namespace cutefish::i18n {
inline std::string tr(const char *text) { return text; }
inline std::string tr(const std::string &text) { return text; }

inline const char *trNoop(const char *text) { return text; }

inline std::string trc(const char *context, const char *text) { return text; }

inline std::string trn(const char *singular, const char *plural, int n) {
    return n == 1 ? singular : plural;
}

inline void setDomain(const char *) {}

template<typename... Args>
inline std::string format(const char *fmt, Args&&...) { return fmt; }

inline std::string format(const std::string &fmt, const std::map<std::string, std::string> &args) {
    return fmt;
}
}

#endif // BASE_I18N_H_
