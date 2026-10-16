// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/common/text_match.h"

#include <algorithm>
#include <string>

#include <unicode/uchar.h>
#include <unicode/ustring.h>

#include "base/strings.h"

namespace cutefish::shell {

static std::u32string folded(std::string_view text)
{
    std::u32string result = base::strings::utf8ToUtf32(text);
    for (char32_t &c : result)
        c = char32_t(u_foldCase(UChar32(c), U_FOLD_CASE_DEFAULT));
    return result;
}

bool containsIgnoringCase(std::string_view text, std::string_view needle)
{
    if (needle.empty())
        return true;
    const std::u32string haystack = folded(text);
    const std::u32string wanted = folded(needle);
    return std::search(haystack.begin(), haystack.end(), wanted.begin(), wanted.end()) != haystack.end();
}

std::string toLower(std::string_view text)
{
    const std::u16string source = base::strings::utf8ToUtf16(text);
    std::u16string result(source.size() + 8, u'\0');
    for (int attempt = 0; attempt < 2; ++attempt) {
        UErrorCode status = U_ZERO_ERROR;
        const int32_t length = u_strToLower(reinterpret_cast<UChar *>(result.data()), int32_t(result.size()),
                                            reinterpret_cast<const UChar *>(source.data()), int32_t(source.size()),
                                            "", &status);
        if (status == U_BUFFER_OVERFLOW_ERROR) {
            result.resize(size_t(length));
            continue;
        }
        if (U_FAILURE(status))
            return std::string(text);
        result.resize(size_t(length));
        return base::strings::utf16ToUtf8(result);
    }
    return std::string(text);
}

} // namespace cutefish::shell
