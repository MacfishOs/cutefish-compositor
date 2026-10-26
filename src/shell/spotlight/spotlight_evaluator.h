// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_SPOTLIGHT_SPOTLIGHT_EVALUATOR_H_
#define CUTEFISH_SHELL_SPOTLIGHT_SPOTLIGHT_EVALUATOR_H_

#include <optional>
#include <string>

namespace cutefish::shell {

struct EvaluationResult {
    bool success = false;
    std::string result;       // Raw formatted answer (e.g. "42", "6.2137 mi")
    std::string formatted;    // Clean display string (e.g. "= 42", "10 km = 6.2137 mi")
    std::string expression;   // Original cleaned expression
    bool isConversion = false;
};

class SpotlightEvaluator {
public:
    static std::optional<EvaluationResult> evaluate(const std::string &input);
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_SPOTLIGHT_SPOTLIGHT_EVALUATOR_H_
