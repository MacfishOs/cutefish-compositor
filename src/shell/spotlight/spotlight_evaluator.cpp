// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/spotlight/spotlight_evaluator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cutefish::shell {

namespace {

std::string trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return std::string(s);
}

std::string toLower(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

std::string formatNumber(double val)
{
    if (std::isnan(val) || std::isinf(val))
        return {};

    if (std::abs(val - std::round(val)) < 1e-9 && std::abs(val) < 1e15) {
        long long intVal = static_cast<long long>(std::round(val));
        std::string s = std::to_string(intVal);
        // Add thousands separator if > 999
        bool neg = (intVal < 0);
        if (neg) s = s.substr(1);
        std::string res;
        int count = 0;
        for (int i = static_cast<int>(s.length()) - 1; i >= 0; --i) {
            res.push_back(s[static_cast<size_t>(i)]);
            if (++count == 3 && i > 0) {
                res.push_back(',');
                count = 0;
            }
        }
        if (neg) res.push_back('-');
        std::reverse(res.begin(), res.end());
        return res;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << val;
    std::string str = oss.str();
    while (str.find('.') != std::string::npos && (str.back() == '0' || str.back() == '.')) {
        bool dot = (str.back() == '.');
        str.pop_back();
        if (dot) break;
    }
    return str;
}

// ---------------------------------------------------------------------------
// Math Evaluator (Recursive descent)
// ---------------------------------------------------------------------------
class MathParser {
public:
    explicit MathParser(std::string expr) : m_expr(std::move(expr)), m_pos(0) {}

    bool parse(double &outValue) {
        skipSpaces();
        if (m_pos >= m_expr.size())
            return false;
        try {
            outValue = parseExpression();
            skipSpaces();
            return m_pos == m_expr.size() && !std::isnan(outValue) && !std::isinf(outValue);
        } catch (...) {
            return false;
        }
    }

private:
    std::string m_expr;
    size_t m_pos = 0;

    void skipSpaces() {
        while (m_pos < m_expr.size() && std::isspace(static_cast<unsigned char>(m_expr[m_pos])))
            ++m_pos;
    }

    char peek() {
        skipSpaces();
        return m_pos < m_expr.size() ? m_expr[m_pos] : '\0';
    }

    char get() {
        skipSpaces();
        return m_pos < m_expr.size() ? m_expr[m_pos++] : '\0';
    }

    double parseExpression() {
        double left = parseTerm();
        while (true) {
            char op = peek();
            if (op == '+' || op == '-') {
                get();
                double right = parseTerm();
                if (op == '+') left += right;
                else left -= right;
            } else {
                break;
            }
        }
        return left;
    }

    double parseTerm() {
        double left = parsePower();
        while (true) {
            char op = peek();
            if (op == '*' || op == '/' || op == '%') {
                get();
                double right = parsePower();
                if (op == '*') left *= right;
                else if (op == '/') {
                    if (std::abs(right) < 1e-12) throw false;
                    left /= right;
                } else if (op == '%') {
                    if (std::abs(right) < 1e-12) throw false;
                    left = std::fmod(left, right);
                }
            } else {
                break;
            }
        }
        return left;
    }

    double parsePower() {
        double left = parseFactor();
        if (peek() == '^') {
            get();
            double right = parsePower();
            return std::pow(left, right);
        }
        return left;
    }

    double parseFactor() {
        skipSpaces();
        char c = peek();
        if (c == '+') {
            get();
            return parseFactor();
        }
        if (c == '-') {
            get();
            return -parseFactor();
        }
        if (c == '(') {
            get();
            double val = parseExpression();
            if (get() != ')') throw false;
            if (peek() == '%') {
                get();
                val /= 100.0;
            }
            return val;
        }

        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            size_t start = m_pos;
            bool hasDot = (c == '.');
            get();
            while (m_pos < m_expr.size()) {
                char ch = m_expr[m_pos];
                if (std::isdigit(static_cast<unsigned char>(ch))) {
                    ++m_pos;
                } else if (ch == '.' && !hasDot) {
                    hasDot = true;
                    ++m_pos;
                } else {
                    break;
                }
            }
            double val = std::stod(m_expr.substr(start, m_pos - start));
            if (peek() == '%') {
                get();
                val /= 100.0;
            }
            return val;
        }

        // Functions / Constants
        if (std::isalpha(static_cast<unsigned char>(c))) {
            size_t start = m_pos;
            while (m_pos < m_expr.size() && std::isalpha(static_cast<unsigned char>(m_expr[m_pos])))
                ++m_pos;
            std::string name = toLower(m_expr.substr(start, m_pos - start));

            if (name == "pi") return M_PI;
            if (name == "e") return M_E;
            if (name == "tau") return 2.0 * M_PI;

            if (peek() == '(') {
                get();
                double arg = parseExpression();
                if (get() != ')') throw false;

                if (name == "sqrt") {
                    if (arg < 0) throw false;
                    return std::sqrt(arg);
                }
                if (name == "cbrt") return std::cbrt(arg);
                if (name == "sin") return std::sin(arg);
                if (name == "cos") return std::cos(arg);
                if (name == "tan") return std::tan(arg);
                if (name == "asin") {
                    if (arg < -1.0 || arg > 1.0) throw false;
                    return std::asin(arg);
                }
                if (name == "acos") {
                    if (arg < -1.0 || arg > 1.0) throw false;
                    return std::acos(arg);
                }
                if (name == "atan") return std::atan(arg);
                if (name == "abs") return std::abs(arg);
                if (name == "log" || name == "log10") {
                    if (arg <= 0) throw false;
                    return std::log10(arg);
                }
                if (name == "ln") {
                    if (arg <= 0) throw false;
                    return std::log(arg);
                }
                if (name == "exp") return std::exp(arg);
                if (name == "round") return std::round(arg);
                if (name == "floor") return std::floor(arg);
                if (name == "ceil") return std::ceil(arg);
            }
        }

        throw false;
    }
};

// ---------------------------------------------------------------------------
// Unit Conversion
// ---------------------------------------------------------------------------
struct UnitDefinition {
    enum class Type { Length, Mass, Temp, Storage, Time, Speed, Currency };
    Type type;
    double toBase; // multiplier to convert to base unit
    std::string canonical;
};

static const std::unordered_map<std::string, UnitDefinition> kUnits = {
    // Length (base = meter)
    {"km", {UnitDefinition::Type::Length, 1000.0, "km"}},
    {"kilometer", {UnitDefinition::Type::Length, 1000.0, "km"}},
    {"kilometers", {UnitDefinition::Type::Length, 1000.0, "km"}},
    {"m", {UnitDefinition::Type::Length, 1.0, "m"}},
    {"meter", {UnitDefinition::Type::Length, 1.0, "m"}},
    {"meters", {UnitDefinition::Type::Length, 1.0, "m"}},
    {"cm", {UnitDefinition::Type::Length, 0.01, "cm"}},
    {"centimeter", {UnitDefinition::Type::Length, 0.01, "cm"}},
    {"centimeters", {UnitDefinition::Type::Length, 0.01, "cm"}},
    {"mm", {UnitDefinition::Type::Length, 0.001, "mm"}},
    {"millimeter", {UnitDefinition::Type::Length, 0.001, "mm"}},
    {"millimeters", {UnitDefinition::Type::Length, 0.001, "mm"}},
    {"mi", {UnitDefinition::Type::Length, 1609.344, "mi"}},
    {"mile", {UnitDefinition::Type::Length, 1609.344, "mi"}},
    {"miles", {UnitDefinition::Type::Length, 1609.344, "mi"}},
    {"yd", {UnitDefinition::Type::Length, 0.9144, "yd"}},
    {"yard", {UnitDefinition::Type::Length, 0.9144, "yd"}},
    {"yards", {UnitDefinition::Type::Length, 0.9144, "yd"}},
    {"ft", {UnitDefinition::Type::Length, 0.3048, "ft"}},
    {"foot", {UnitDefinition::Type::Length, 0.3048, "ft"}},
    {"feet", {UnitDefinition::Type::Length, 0.3048, "ft"}},
    {"in", {UnitDefinition::Type::Length, 0.0254, "in"}},
    {"inch", {UnitDefinition::Type::Length, 0.0254, "in"}},
    {"inches", {UnitDefinition::Type::Length, 0.0254, "in"}},

    // Mass (base = gram)
    {"kg", {UnitDefinition::Type::Mass, 1000.0, "kg"}},
    {"kilogram", {UnitDefinition::Type::Mass, 1000.0, "kg"}},
    {"kilograms", {UnitDefinition::Type::Mass, 1000.0, "kg"}},
    {"g", {UnitDefinition::Type::Mass, 1.0, "g"}},
    {"gram", {UnitDefinition::Type::Mass, 1.0, "g"}},
    {"grams", {UnitDefinition::Type::Mass, 1.0, "g"}},
    {"mg", {UnitDefinition::Type::Mass, 0.001, "mg"}},
    {"lb", {UnitDefinition::Type::Mass, 453.59237, "lb"}},
    {"lbs", {UnitDefinition::Type::Mass, 453.59237, "lb"}},
    {"pound", {UnitDefinition::Type::Mass, 453.59237, "lb"}},
    {"pounds", {UnitDefinition::Type::Mass, 453.59237, "lb"}},
    {"oz", {UnitDefinition::Type::Mass, 28.349523, "oz"}},
    {"ounce", {UnitDefinition::Type::Mass, 28.349523, "oz"}},
    {"ounces", {UnitDefinition::Type::Mass, 28.349523, "oz"}},

    // Temperature
    {"c", {UnitDefinition::Type::Temp, 1.0, "°C"}},
    {"celsius", {UnitDefinition::Type::Temp, 1.0, "°C"}},
    {"f", {UnitDefinition::Type::Temp, 1.0, "°F"}},
    {"fahrenheit", {UnitDefinition::Type::Temp, 1.0, "°F"}},
    {"k", {UnitDefinition::Type::Temp, 1.0, "K"}},
    {"kelvin", {UnitDefinition::Type::Temp, 1.0, "K"}},

    // Storage (base = byte)
    {"b", {UnitDefinition::Type::Storage, 1.0, "B"}},
    {"byte", {UnitDefinition::Type::Storage, 1.0, "B"}},
    {"bytes", {UnitDefinition::Type::Storage, 1.0, "B"}},
    {"kb", {UnitDefinition::Type::Storage, 1024.0, "KB"}},
    {"mb", {UnitDefinition::Type::Storage, 1024.0 * 1024.0, "MB"}},
    {"gb", {UnitDefinition::Type::Storage, 1024.0 * 1024.0 * 1024.0, "GB"}},
    {"tb", {UnitDefinition::Type::Storage, 1024.0 * 1024.0 * 1024.0 * 1024.0, "TB"}},

    // Time (base = second)
    {"s", {UnitDefinition::Type::Time, 1.0, "s"}},
    {"sec", {UnitDefinition::Type::Time, 1.0, "s"}},
    {"second", {UnitDefinition::Type::Time, 1.0, "s"}},
    {"seconds", {UnitDefinition::Type::Time, 1.0, "s"}},
    {"min", {UnitDefinition::Type::Time, 60.0, "min"}},
    {"minute", {UnitDefinition::Type::Time, 60.0, "min"}},
    {"minutes", {UnitDefinition::Type::Time, 60.0, "min"}},
    {"h", {UnitDefinition::Type::Time, 3600.0, "hr"}},
    {"hr", {UnitDefinition::Type::Time, 3600.0, "hr"}},
    {"hour", {UnitDefinition::Type::Time, 3600.0, "hr"}},
    {"hours", {UnitDefinition::Type::Time, 3600.0, "hr"}},
    {"day", {UnitDefinition::Type::Time, 86400.0, "days"}},
    {"days", {UnitDefinition::Type::Time, 86400.0, "days"}},

    // Currency (relative to USD)
    {"usd", {UnitDefinition::Type::Currency, 1.0, "USD"}},
    {"eur", {UnitDefinition::Type::Currency, 1.08, "EUR"}},
    {"gbp", {UnitDefinition::Type::Currency, 1.28, "GBP"}},
    {"jpy", {UnitDefinition::Type::Currency, 0.0066, "JPY"}},
    {"inr", {UnitDefinition::Type::Currency, 0.012, "INR"}},
    {"cad", {UnitDefinition::Type::Currency, 0.72, "CAD"}},
    {"aud", {UnitDefinition::Type::Currency, 0.66, "AUD"}},
};

std::optional<EvaluationResult> tryConversion(const std::string &input)
{
    std::string s = toLower(trim(input));
    std::string separator;
    size_t sepPos = s.find(" to ");
    if (sepPos != std::string::npos) {
        separator = " to ";
    } else {
        sepPos = s.find(" in ");
        if (sepPos != std::string::npos)
            separator = " in ";
    }

    if (sepPos == std::string::npos)
        return std::nullopt;

    std::string leftStr = trim(s.substr(0, sepPos));
    std::string targetUnit = trim(s.substr(sepPos + separator.length()));

    // Extract value and source unit from leftStr (e.g. "10 km" or "10km")
    size_t numEnd = 0;
    while (numEnd < leftStr.size() && (std::isdigit(static_cast<unsigned char>(leftStr[numEnd])) || leftStr[numEnd] == '.' || leftStr[numEnd] == '-'))
        ++numEnd;

    if (numEnd == 0)
        return std::nullopt;

    double value = 0.0;
    try {
        value = std::stod(leftStr.substr(0, numEnd));
    } catch (...) {
        return std::nullopt;
    }

    std::string sourceUnit = trim(leftStr.substr(numEnd));
    if (sourceUnit.empty() || targetUnit.empty())
        return std::nullopt;

    auto itSrc = kUnits.find(sourceUnit);
    auto itDst = kUnits.find(targetUnit);
    if (itSrc == kUnits.end() || itDst == kUnits.end())
        return std::nullopt;

    if (itSrc->second.type != itDst->second.type)
        return std::nullopt;

    double converted = 0.0;
    if (itSrc->second.type == UnitDefinition::Type::Temp) {
        // Temperature conversion
        std::string src = itSrc->second.canonical;
        std::string dst = itDst->second.canonical;
        double c = value;
        if (src == "°F") c = (value - 32.0) * 5.0 / 9.0;
        else if (src == "K") c = value - 273.15;

        if (dst == "°C") converted = c;
        else if (dst == "°F") converted = (c * 9.0 / 5.0) + 32.0;
        else if (dst == "K") converted = c + 273.15;
    } else {
        // Standard linear conversion
        double baseValue = value * itSrc->second.toBase;
        converted = baseValue / itDst->second.toBase;
    }

    EvaluationResult res;
    res.success = true;
    res.isConversion = true;
    res.result = formatNumber(converted) + " " + itDst->second.canonical;
    res.formatted = formatNumber(value) + " " + itSrc->second.canonical + " = " + res.result;
    res.expression = input;
    return res;
}

} // namespace

std::optional<EvaluationResult> SpotlightEvaluator::evaluate(const std::string &rawInput)
{
    std::string input = trim(rawInput);
    if (input.empty())
        return std::nullopt;

    // 1. Try unit conversion
    if (auto conv = tryConversion(input))
        return conv;

    // 2. Try math evaluation
    // Pre-filter: must contain at least one math operator or valid function/constant
    bool hasOperator = false;
    for (char c : input) {
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '^' || c == '(' || c == ')') {
            hasOperator = true;
            break;
        }
    }

    std::string lower = toLower(input);
    bool hasFunction = (lower.starts_with("sqrt(") || lower.starts_with("cbrt(") ||
                        lower.starts_with("sin(") || lower.starts_with("cos(") ||
                        lower.starts_with("tan(") || lower.starts_with("abs(") ||
                        lower.starts_with("log(") || lower.starts_with("ln("));

    // Handle "X% of Y"
    size_t ofPos = lower.find(" of ");
    if (ofPos != std::string::npos && ofPos > 0 && lower[ofPos - 1] == '%') {
        std::string pStr = trim(input.substr(0, ofPos - 1));
        std::string yStr = trim(input.substr(ofPos + 4));
        input = "(" + pStr + " / 100.0) * (" + yStr + ")";
        hasOperator = true;
    }

    if (!hasOperator && !hasFunction)
        return std::nullopt;

    // Normalize characters: replace unicode or alternate multiply/divide
    std::string sanitized;
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == 'x' || c == 'X') {
            // Check if 'x' is used as multiplication operator (surrounded by digits/spaces)
            if (i > 0 && (std::isdigit(static_cast<unsigned char>(input[i-1])) || std::isspace(static_cast<unsigned char>(input[i-1])))) {
                sanitized.push_back('*');
                continue;
            }
        }
        sanitized.push_back(c);
    }

    MathParser parser(sanitized);
    double value = 0.0;
    if (parser.parse(value)) {
        EvaluationResult res;
        res.success = true;
        res.isConversion = false;
        std::string numStr = formatNumber(value);
        if (numStr.empty())
            return std::nullopt;
        res.result = numStr;
        res.formatted = "= " + numStr;
        res.expression = rawInput;
        return res;
    }

    return std::nullopt;
}

} // namespace cutefish::shell
