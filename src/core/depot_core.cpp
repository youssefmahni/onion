#include "depot_core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <unordered_set>

namespace depot {
int policy_target(int cores, double load1, double ram_avail_gb, double net_mbps,
                  double link_mbps, double gpu_pct, int prev) {
    int base = cores < 2 ? 2 : (cores > 16 ? 16 : cores);
    int t = prev < 1 ? 1 : (prev > 5000 ? 5000 : prev);
    if (load1 > 0.8 * cores) t -= 2;          // user busy -> back off
    else if (load1 < 0.3 * cores) t += 2;     // idle -> more workers
    if (t > base && load1 < 0.3 * cores) t = base;  // cap idle growth at base
    if (ram_avail_gb < 1.0) t /= 2;           // memory pressure
    if (link_mbps > 0 && net_mbps > 0.7 * link_mbps) t = t < 4 ? t : 4;  // NIC saturated
    if (gpu_pct > 90.0) t = t < 2 ? t : 2;    // GPU busy
    if (t < 1) t = 1;
    if (t > 5000) t = 5000;
    return t;
}



static const std::unordered_set<std::string> FILLER = {
    "the","a","an","of","and","or","for","to","in","on","at","with","by","from",
    "vs","versus","into","within"};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

std::string lower(const std::string& s) {
    std::string t = s;
    for (char& c : t) c = (char)std::tolower((unsigned char)c);
    return t;
}

bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// minimal CSV line splitter (honours double quotes)
std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur; bool q = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (q) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
            else if (c == '"') q = false;
            else cur += c;
        } else if (c == '"') q = true;
        else if (c == ',') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::vector<std::string> split_pipe(const std::string& s) {
    std::vector<std::string> out;
    if (trim(s).empty()) return out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('|', i);
        if (j == std::string::npos) {
            std::string last = trim(s.substr(i));
            if (!last.empty()) out.push_back(last);
            break;
        }
        std::string part = trim(s.substr(i, j - i));
        if (!part.empty()) out.push_back(part);
        i = j + 1;
    }
    return out;
}

std::string sing(const std::string& w) {
    if (w.size() <= 3) return w;
    if (ends_with(w, "ies")) return w.substr(0, w.size() - 3) + "y";
    if (ends_with(w, "ss") || ends_with(w, "us") || ends_with(w, "is")) return w;
    if (ends_with(w, "ses") || ends_with(w, "ches") || ends_with(w, "shes")) return w;
    if (ends_with(w, "es")) return w.substr(0, w.size() - 2);
    if (ends_with(w, "s")) return w.substr(0, w.size() - 1);
    return w;
}

std::string canonical(const std::string& title) {
    std::string t = lower(title);
    size_t p;
    while ((p = t.find('&')) != std::string::npos) t.replace(p, 1, " and ");
    for (char& c : t) if (!std::isalnum((unsigned char)c)) c = ' ';
    std::istringstream ss(t);
    std::vector<std::string> toks; std::string w;
    while (ss >> w) if (!FILLER.count(w)) toks.push_back(sing(w));
    std::sort(toks.begin(), toks.end());
    std::string out;
    for (size_t i = 0; i < toks.size(); ++i) out += (i ? " " : "") + toks[i];
    return out;
}

// LCS-based ratio approximating difflib.SequenceMatcher.ratio
double lcs_ratio(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return 0.0;
    size_t n = a.size(), m = b.size();
    std::vector<std::vector<int>> dp(2, std::vector<int>(m + 1, 0));
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j)
            dp[i & 1][j] = (a[i - 1] == b[j - 1]) ? dp[(i & 1) ^ 1][j - 1] + 1
                                                 : std::max(dp[(i & 1) ^ 1][j], dp[i & 1][j - 1]);
    }
    int lcs = dp[n & 1][m];
    return 2.0 * lcs / (double)(n + m);
}

double jaccard_tokens(const std::string& a, const std::string& b) {
    std::istringstream sa(a), sb(b);
    std::unordered_set<std::string> ta, tb; std::string w;
    while (sa >> w) ta.insert(w);
    while (sb >> w) tb.insert(w);
    if (ta.empty() && tb.empty()) return 0.0;
    size_t inter = 0;
    for (const auto& t : ta) if (tb.count(t)) ++inter;
    return (double)inter / (double)(ta.size() + tb.size() - inter);
}

double sim_keys(const std::string& k1, const std::string& k2) {
    if (k1.empty() || k2.empty()) return 0.0;
    if (k1 == k2) return 1.0;
    return std::max(lcs_ratio(k1, k2), jaccard_tokens(k1, k2));
}

std::vector<std::string> style_flags(const std::string& title) {
    std::vector<std::string> f;
    if (title.find('?') != std::string::npos || title.find('!') != std::string::npos)
        f.push_back("question_or_bang");
    if (title.empty() || !std::isupper((unsigned char)title[0])) f.push_back("not_title_case");
    size_t words = 0; std::istringstream ss(title); std::string w;
    while (ss >> w) ++words;
    if (words > 8) f.push_back("too_many_words");
    if (!title.empty() && (title.back() == '.' || title.back() == ':')) f.push_back("trailing_punct");
    if (words > 2) {
        std::istringstream s2(title); std::string x; bool filler = false;
        while (s2 >> x) if (FILLER.count(lower(x))) filler = true;
        if (filler) f.push_back("contains_filler");
    }
    return f;
}

std::vector<int> parse_levels(const std::string& spec) {
    std::vector<int> out;
    size_t i = 0;
    while (i <= spec.size()) {
        size_t j = spec.find(',', i);
        std::string tok = trim(j == std::string::npos ? spec.substr(i) : spec.substr(i, j - i));
        if (!tok.empty() && (tok[0] == 'L' || tok[0] == 'l')) tok = tok.substr(1);
        if (tok.empty()) return {};  // e.g. ",," or ""
        for (char c : tok) if (!std::isdigit((unsigned char)c)) return {};
        int v = std::atoi(tok.c_str());
        if (v < 0 || v > 9) return {};
        out.push_back(v);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return out;
}

std::string sanitize_tsv(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) o += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    return o;
}

int next_tk(const std::vector<Row>& l5, const std::vector<Row>& l6) {
    int mx = 0;
    auto bump = [&mx](const std::string& id) {
        if (id.size() > 2 && id[0] == 'T' && id[1] == 'K') {
            int v = std::atoi(id.c_str() + 2);
            mx = std::max(mx, v);
        }
    };
    for (const auto& r : l5) bump(r.id);
    for (const auto& r : l6) bump(r.id);
    return mx + 1;
}

std::string td_prefix(const std::string& path) {
    size_t cnt = 0, i = 0;
    for (; i < path.size(); ++i) if (path[i] == '-') { if (++cnt == 5) break; }
    return path.substr(0, i);
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\t': o += "\\t"; break;
            case '\r': o += "\\r"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", (unsigned char)c);
                    o += buf;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

// parse a quoted JSON string starting at pos (js[pos]=='"'); returns (value, index past closing quote)
// handles the full JSON escape set: \" \\ \n \t \r \uXXXX (ASCII only)
static std::pair<std::string, size_t> parse_quoted(const std::string& js, size_t pos) {
    std::string out;
    size_t i = pos + 1;
    while (i < js.size() && js[i] != '"') {
        if (js[i] == '\\' && i + 1 < js.size()) {
            char n = js[i + 1];
            switch (n) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'u':
                    if (i + 5 < js.size()) {
                        char buf[5] = {js[i + 2], js[i + 3], js[i + 4], js[i + 5], 0};
                        int code = (int)std::strtol(buf, nullptr, 16);
                        if (code > 0 && code < 0x80) out += (char)code;
                        i += 4;
                    }
                    break;
                default: out += n;
            }
            i += 2;
        } else { out += js[i]; i++; }
    }
    return {out, std::min(i + 1, js.size())};
}

// extract the JSON object inside "content" of a chat-completions body.
// On the real wire, content is a JSON string with escaped quotes
// (e.g. "content":"{\"items\": [...]}"); we unescape it first. Raw
// (already-decoded) content is tolerated as a fallback for tests.
static bool content_json(const std::string& body, std::string& js) {
    size_t p = body.find("\"content\"");
    if (p == std::string::npos) return false;
    size_t colon = body.find(':', p);
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    while (i < body.size() && std::isspace((unsigned char)body[i])) i++;
    if (i < body.size() && body[i] == '"') {
        auto pr = parse_quoted(body, i);
        js = pr.first;  // unescaped content string
    } else {
        js = body.substr(i);  // raw fallback
    }
    size_t b = js.find('{');
    size_t e = js.rfind('}');
    if (b == std::string::npos || e == std::string::npos || e <= b) return false;
    js = js.substr(b, e - b + 1);
    return true;
}

static void skip_ws(const std::string& js, size_t& i) {
    while (i < js.size() && std::isspace((unsigned char)js[i])) i++;
}

// parse the string array value of a named key inside js; returns items or empty
static std::vector<std::string> key_str_array(const std::string& js, const std::string& key) {
    std::vector<std::string> out;
    size_t q = js.find("\"" + key + "\"");
    if (q == std::string::npos) return out;
    size_t colon = js.find(':', q);
    size_t i = js.find('[', colon + 1);
    if (i == std::string::npos) return out;
    i++;
    while (i < js.size()) {
        skip_ws(js, i);
        if (i >= js.size() || js[i] == ']') return out;
        if (js[i] == '"') {
            auto pr = parse_quoted(js, i);
            out.push_back(pr.first);
            i = pr.second;
        } else {
            while (i < js.size() && js[i] != ',' && js[i] != ']') i++;
        }
        skip_ws(js, i);
        if (i < js.size() && js[i] == ',') i++;
    }
    return out;
}

FunnelCritique parse_funnel_c(const std::string& body) {
    FunnelCritique out;
    std::string js;
    if (!content_json(body, js)) return out;
    size_t q = js.find("\"score\"");
    if (q != std::string::npos) {
        size_t colon = js.find(':', q);
        size_t i = colon + 1;
        while (i < js.size() && std::isspace((unsigned char)js[i])) i++;
        char* endp = nullptr;
        double v = std::strtod(js.c_str() + i, &endp);
        if (endp && endp != js.c_str() + i && v >= 0.0 && v <= 1.0) out.score = v;
    }
    out.remove = key_str_array(js, "remove");
    out.add = key_str_array(js, "add");
    size_t iq = js.find("\"issues\"");
    if (iq != std::string::npos) {
        size_t colon = js.find(':', iq);
        size_t s1 = js.find('"', colon + 1);
        if (s1 != std::string::npos) {
            out.issues = parse_quoted(js, s1).first;
            // could be JSON null
            if (out.issues == "null") out.issues.clear();
        }
    }
    return out;
}

std::vector<std::string> parse_funnel_r1(const std::string& body) {
    std::vector<std::string> out;
    std::string js;
    if (!content_json(body, js)) return out;
    size_t q = js.find("\"items\"");
    if (q == std::string::npos) return out;
    size_t colon = js.find(':', q);
    size_t i = js.find('[', colon + 1);
    if (i == std::string::npos) return out;
    i++;
    while (i < js.size()) {
        skip_ws(js, i);
        if (i >= js.size() || js[i] == ']') return out;
        if (js[i] == '"') {
            auto pr = parse_quoted(js, i);
            out.push_back(pr.first);
            i = pr.second;
        } else {
            while (i < js.size() && js[i] != ',' && js[i] != ']') i++;
        }
        skip_ws(js, i);
        if (i < js.size() && js[i] == ',') i++;
    }
    return out;
}

std::vector<FunnelItem> parse_funnel_r4(const std::string& body) {
    std::vector<FunnelItem> out;
    std::string js;
    if (!content_json(body, js)) return out;
    size_t q = js.find("\"items\"");
    if (q == std::string::npos) return out;
    size_t colon = js.find(':', q);
    size_t i = js.find('[', colon + 1);
    if (i == std::string::npos) return out;
    i++;
    while (i < js.size()) {
        skip_ws(js, i);
        if (i >= js.size() || js[i] == ']') return out;
        if (js[i] != '{') {
            while (i < js.size() && js[i] != ',' && js[i] != ']') i++;
            skip_ws(js, i);
            if (i < js.size() && js[i] == ',') i++;
            continue;
        }
        // scan the object to its matching '}' while respecting strings
        size_t j = i + 1;
        int depth = 1;
        bool in_str = false;
        while (j < js.size() && depth > 0) {
            char c = js[j];
            if (in_str) {
                if (c == '\\') j++;
                else if (c == '"') in_str = false;
            } else if (c == '"') in_str = true;
            else if (c == '{') depth++;
            else if (c == '}') depth--;
            j++;
        }
        if (depth != 0) return out;  // malformed: unclosed object
        std::string obj = js.substr(i, j - i);
        FunnelItem it;
        size_t tq = obj.find("\"title\"");
        if (tq != std::string::npos) {
            size_t c2 = obj.find(':', tq);
            size_t s1 = obj.find('"', c2 + 1);
            if (s1 != std::string::npos) it.title = parse_quoted(obj, s1).first;
        }
        size_t aq = obj.find("\"atomic\"");
        if (aq != std::string::npos) {
            size_t c2 = obj.find(':', aq);
            size_t t = obj.find("true", c2), f = obj.find("false", c2);
            if (t != std::string::npos && (f == std::string::npos || t < f)) it.atomic = true;
        }
        i = j;
        skip_ws(js, i);
        if (i < js.size() && js[i] == ',') i++;
        if (it.title.empty()) continue;  // skip malformed object, keep parsing the rest
        out.push_back(it);
    }
    return out;
}

LlmAnswer parse_llm(const std::string& body) {
    LlmAnswer a;
    std::string js;
    if (!content_json(body, js)) {
        a.error = "no content field: " + body.substr(0, 120);
        return a;
    }
    auto getbool = [&](const std::string& k, bool def) {
        size_t q = js.find("\"" + k + "\"");
        if (q == std::string::npos) return def;
        size_t v = js.find(':', q);
        if (v == std::string::npos) return def;
        size_t t = js.find("true", v), f = js.find("false", v);
        size_t close = js.find('}', q);
        if (t != std::string::npos && t < close && (f == std::string::npos || t < f)) return true;
        if (f != std::string::npos && f < close) return false;
        return def;
    };
    a.valid = getbool("valid", true);
    a.admits = getbool("admits_children", true);
    size_t q = js.find("\"fixed\"");
    if (q != std::string::npos) {
        size_t v = js.find(':', q);
        size_t nstart = v + 1;
        while (nstart < js.size() && std::isspace((unsigned char)js[nstart])) nstart++;
        if (js.compare(nstart, 4, "null") != 0) {   // only parse if not the JSON null literal
            size_t s1 = js.find('"', nstart);
            if (s1 != std::string::npos) {
                size_t s2 = s1 + 1;
                while (s2 < js.size() && js[s2] != '"') { if (js[s2] == '\\') s2++; s2++; }
                a.fixed = js.substr(s1 + 1, s2 - s1 - 1);
            }
        }
    }
    a.parse_ok = true;
    return a;
}

bool reply_truncated(const std::string& body) {
    static const std::string key = "\"finish_reason\"";
    size_t p = 0;
    while ((p = body.find(key, p)) != std::string::npos) {
        // an escaped occurrence (\"finish_reason\") lives inside message
        // content, not in the envelope: ignore it
        if (p > 0 && body[p - 1] == '\\') { p += key.size(); continue; }
        size_t q = p + key.size();
        while (q < body.size() &&
               (body[q] == ' ' || body[q] == ':' || body[q] == '\t' ||
                body[q] == '\n' || body[q] == '\r'))
            ++q;
        if (body.compare(q, 8, "\"length\"") == 0) return true;
        p = q;
    }
    return false;
}

}  // namespace depot

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
extern "C" {
EMSCRIPTEN_KEEPALIVE int wasm_policy_target(int cores, double load1, double ram_avail_gb,
                                            double net_mbps, double link_mbps, double gpu_pct,
                                            int prev) {
    return depot::policy_target(cores, load1, ram_avail_gb, net_mbps, link_mbps, gpu_pct, prev);
}
}
#endif
