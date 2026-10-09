// demo_preview.cpp — trace exactly what happens in ONE funnel loop (one node).
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "depot_core.hpp"
#include "depot_io.hpp"

using std::string;

// extract + unescape the user-message content from a payload for display
static string extract_user(const string& payload) {
    size_t p = payload.rfind("\"content\":\"");
    if (p == string::npos) return payload;
    size_t i = p + 11;
    string out;
    while (i < payload.size() && payload[i] != '"') {
        if (payload[i] == '\\' && i + 1 < payload.size()) { out += payload[i + 1]; i += 2; }
        else { out += payload[i]; i++; }
    }
    return out;
}

static void show(const string& role, const string& payload) {
    string u = extract_user(payload);
    if (u.size() > 420) u = u.substr(0, 420) + " …";
    std::cout << "\n════ " << role << " — prompt sent:\n" << u << "\n";
}

static string wire(const string& inner) {  // wrap inner JSON into a real API body
    string esc;
    for (char c : inner) {
        if (c == '"') esc += "\\\"";
        else esc += c;
    }
    return "{\"choices\":[{\"message\":{\"content\":\"" + esc + "\"}}]}";
}

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "depot_preview_demo";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto wf = [&](const string& n, const string& c) {
        std::ofstream f(dir / n); f << c;
    };
    wf("Onion - level_00.csv", "ID,Phase\nP01,Identify Problem or Opportunity\n");
    wf("Onion - level_01.csv", "ID,ID-L1,DOC\nIN01,P01-IN01,Identify Problem or Opportunity\n");
    wf("Onion - level_02.csv", "ID,ID-L2,Tasks & Activities\nT01,P01-IN01-T01,Business Discovery\n");
    wf("Onion - level_03.csv", "ID,ID-L3,Title\nST01,P01-IN01-T01-ST01,Business Context\n");
    wf("Onion - level_04.csv", "ID,ID-L4,Title\nTD1,P01-IN01-T01-ST01-TD1,Define business domain\n");
    wf("Onion - level_05.csv", "ID,ID-L5,Title\nTK0001,P01-IN01-T01-ST01-TD1-TK0001,Existing Item\n");
    for (int l = 6; l <= 9; ++l)
        wf("Onion - level_0" + std::to_string(l) + ".csv", "ID,ID-L" + std::to_string(l) + ",Title\n");

    int call = 0;
    depot::AskFn fake = [&](const string& payload) -> string {
        int n = call++;
        if (payload.find("Plan the knowledge coverage") != string::npos) {
            show("PLANNER (1 call)", payload);
            std::cout << "     LLM answers: {\"items\": [\"Operations\", \"Governance\"]}  (2 aspects)\n";
            return wire("{\"items\": [\"Operations\", \"Governance\"]}");
        }
        if (payload.find("Aspect:") != string::npos) {
            string aspect = payload.find("Aspect: Operations") != string::npos ? "Operations" : "Governance";
            show("WORKER (parallel, per aspect) — " + aspect, payload);
            if (aspect == "Operations") {
                std::cout << "     LLM answers: 3 candidates\n";
                return wire("{\"items\": [\"Order Capture\", \"Order Validation\", \"Order Fulfillment\"]}");
            }
            std::cout << "     LLM answers: 2 candidates\n";
            return wire("{\"items\": [\"Approval Workflow\", \"Policy Enforcement\"]}");
        }
        if (payload.find("Critique this candidate set") != string::npos) {
            show("CRITIQUE (1 call)", payload);
            std::cout << "     LLM answers: score 0.85, add [Order Tracking], issues: 'consider returns handling'\n";
            return wire("{\"score\": 0.85, \"remove\": [], \"add\": [\"Order Tracking\"], \"issues\": \"consider returns handling\"}");
        }
        if (payload.find("select the final children set") != string::npos) {
            show("SYNTHESIZER (1 call)", payload);
            std::cout << "     LLM answers: 3 final items (1 nests further, 2 atomic; 'Existing Item' filtered)\n";
            return wire("{\"items\": [{\"title\": \"Order Capture\", \"atomic\": false},"
                        "{\"title\": \"Order Tracking\", \"atomic\": true},"
                        "{\"title\": \"Approval Workflow\", \"atomic\": true}]}");
        }
        return wire("{\"items\": []}");
    };

    std::cout << "=== ONE LOOP on node: L4 P01-IN01-T01-ST01-TD1 \"Define business domain\" ===\n";
    std::cout << "=== (trim rounds 400/300 are SKIPPED here: pool 5 <= 400) ===\n";
    depot::run_funnel("P01-IN01-", 1, 2, false, true, "deepseek-chat", "sk-fake",
                      dir.string(), fake);

    std::cout << "\n════ WRITES: level_05.csv after the loop:\n";
    std::ifstream f(dir / "Onion - level_05.csv");
    string line;
    while (std::getline(f, line)) std::cout << "  " << line << "\n";
    std::cout << "\n════ funnel_state.tsv (resume marker):\n";
    std::ifstream st(dir / "funnel_state.tsv");
    while (std::getline(st, line)) std::cout << "  " << line << "\n";
    std::cout << "\n════ funnel_critique.tsv (audit):\n";
    std::ifstream cr(dir / "funnel_critique.tsv");
    while (std::getline(cr, line)) std::cout << "  " << line << "\n";
    std::cout << "\nnote: 'Order Capture' (atomic=false) would be re-funnelled in the NEXT loop "
                 "to nest one level deeper (limit=1 stops here).\n";
    fs::remove_all(dir);
    return 0;
}
