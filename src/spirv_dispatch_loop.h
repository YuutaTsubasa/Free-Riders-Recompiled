#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sfr {

// The same vertex shader without XenosRecomp's dispatch loop. A guest shader
// with jumps or loops becomes "uint pc = 0; while (true) switch (pc) {...}":
// every case falls through to the next or sets pc and continues. In SPIR-V
// that is a loop whose header carries every guest register as a phi into a
// switch on pc. Apple's Metal compiler (MoltenVK) spills 6-8 KB a thread on
// the skinned shaders of that shape, and a race spends ~130 ms a frame in
// vertex work; the same cases run in order, each behind its own test of pc,
// spill next to nothing.
//
// So the cases become guarded regions in case order: "switch (pc) { case n:
// ... }" alone, its exits setting the next case (the following one, the one a
// continue names, or none for a break) and leaving to the guard's merge. A
// loop goes only around the cases a backward continue spans. The values the
// old loop carried, and every value used outside its own block, live in
// Function variables (Metal's compiler turns them back into registers). Each
// original instruction runs in the same order as before, so the results are
// the same, NoContraction and all.
//
// Empty, with the reason in why, for a module this does not understand; a
// module without the dispatch loop comes back as it is.
inline std::optional<std::vector<uint8_t>> without_dispatch_loop_spirv(std::span<const uint8_t> code,
                                                                       std::string* why = nullptr) {
    using Words = std::vector<uint32_t>;
    const auto refuse = [&](const std::string& reason) -> std::optional<std::vector<uint8_t>> {
        if (why) *why = reason;
        return std::nullopt;
    };
    if (code.size() < 20 || code.size() % 4) return refuse("not a SPIR-V module");
    std::vector<uint32_t> words(code.size() / 4);
    std::memcpy(words.data(), code.data(), code.size());
    if (words[0] != 0x07230203u) return refuse("not a SPIR-V module (magic)");

    enum : uint32_t {
        op_name = 5, op_member_name = 6, op_ext_inst = 12, op_type_bool = 20, op_type_int = 21, op_type_pointer = 32,
        op_constant = 43, op_function = 54, op_function_parameter = 55, op_function_end = 56, op_variable = 59,
        op_load = 61, op_store = 62, op_access_chain = 65, op_in_bounds_access_chain = 66, op_decorate = 71,
        op_member_decorate = 72, op_vector_shuffle = 79, op_composite_extract = 81, op_composite_insert = 82,
        op_logical_and = 167, op_u_greater_than_equal = 174, op_u_less_than_equal = 178, op_phi = 245,
        op_loop_merge = 246, op_selection_merge = 247, op_label = 248, op_branch = 249, op_branch_conditional = 250,
        op_switch = 251, op_kill = 252, op_return = 253, op_return_value = 254, op_unreachable = 255,
    };
    constexpr uint32_t storage_function = 7;
    // Instructions whose operands after the result type and id are all ids.
    static const std::unordered_set<uint32_t> all_ids = {
        65,  66,  80,  83,  109, 110, 111, 112, 124, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138,
        139, 140, 141, 142, 144, 145, 148, 154, 155, 156, 157, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 175,
        174, 176, 177, 178, 179, 180, 182, 183, 184, 186, 188, 190, 194, 195, 196, 197, 198, 199, 200};

    // Positions of the id operands (not labels) an instruction in a function
    // body reads; false for an instruction this does not know.
    const auto id_operands = [&](const Words& in, std::vector<size_t>& at) {
        at.clear();
        const uint32_t opcode = in[0] & 0xffff, count = uint32_t(in.size());
        if (all_ids.contains(opcode)) {
            for (size_t k = 3; k < count; ++k) at.push_back(k);
            return count >= 3;
        }
        switch (opcode) {
        case op_load: if (count >= 4) at.push_back(3); return count >= 4;
        case op_store: if (count >= 3) { at.push_back(1); at.push_back(2); } return count >= 3;
        case op_vector_shuffle: if (count >= 5) { at.push_back(3); at.push_back(4); } return count >= 5;
        case op_composite_extract: if (count >= 4) at.push_back(3); return count >= 4;
        case op_composite_insert: if (count >= 5) { at.push_back(3); at.push_back(4); } return count >= 5;
        case op_ext_inst: for (size_t k = 5; k < count; ++k) at.push_back(k); return count >= 5;
        case op_branch_conditional: if (count >= 4) at.push_back(1); return count >= 4;
        case op_switch: if (count >= 3) at.push_back(1); return count >= 3;
        case op_return_value: if (count >= 2) at.push_back(1); return count >= 2;
        case op_phi: case op_loop_merge: case op_selection_merge: case op_label: case op_branch: case op_kill:
        case op_return: case op_unreachable: case op_variable: return true;
        default: return false;
        }
    };
    const auto has_result = [&](const Words& in) {
        const uint32_t opcode = in[0] & 0xffff;
        return all_ids.contains(opcode) || opcode == op_load || opcode == op_vector_shuffle ||
               opcode == op_composite_extract || opcode == op_composite_insert || opcode == op_ext_inst ||
               opcode == op_phi || opcode == op_variable;
    };
    const auto make = [](uint32_t opcode, std::initializer_list<uint32_t> operands) {
        Words in{uint32_t(operands.size() + 1) << 16 | opcode};
        in.insert(in.end(), operands);
        return in;
    };
    const auto is_terminator = [](uint32_t opcode) {
        return opcode == op_branch || opcode == op_branch_conditional || opcode == op_switch || opcode == op_kill ||
               opcode == op_return || opcode == op_return_value || opcode == op_unreachable;
    };

    // The module: what comes before the function, the function's blocks.
    std::vector<Words> globals, after;
    struct Block {
        uint32_t label;
        std::vector<Words> body;  // without the OpLabel; merge and terminator last
    };
    Words function_head;  // OpFunction (and no parameters: refused)
    std::vector<Block> blocks;
    bool in_function = false, seen_function = false;
    for (size_t i = 5; i < words.size();) {
        const uint32_t count = words[i] >> 16, opcode = words[i] & 0xffff;
        if (!count || i + count > words.size()) return refuse("truncated instruction");
        Words in(words.begin() + i, words.begin() + i + count);
        i += count;
        if (opcode == op_function) {
            if (seen_function) {
                // More than one function: leave it if none has a switch.
                for (size_t k = i; k < words.size();) {
                    const uint32_t c = words[k] >> 16;
                    if (!c) break;
                    if ((words[k] & 0xffff) == op_switch) return refuse("more than one function");
                    k += c;
                }
                return std::vector<uint8_t>(code.begin(), code.end());
            }
            seen_function = in_function = true;
            function_head = std::move(in);
            continue;
        }
        if (!in_function) {
            (seen_function ? after : globals).push_back(std::move(in));
            continue;
        }
        if (opcode == op_function_parameter) return refuse("a function with parameters");
        if (opcode == op_function_end) {
            in_function = false;
            continue;
        }
        if (opcode == op_label) {
            blocks.push_back({in[1], {}});
            continue;
        }
        if (blocks.empty()) return refuse("an instruction outside a block");
        blocks.back().body.push_back(std::move(in));
    }
    if (blocks.empty()) return std::vector<uint8_t>(code.begin(), code.end());

    // The dispatch loop: a loop header H branching to D, D a switch on a phi
    // of H; the switch merges at S, which leaves the loop for its merge M.
    std::unordered_map<uint32_t, size_t> index;  // label -> block
    for (size_t b = 0; b < blocks.size(); ++b) index[blocks[b].label] = b;
    const auto terminator = [](Block& block) -> Words& { return block.body.back(); };
    const auto merge_of = [](Block& block) -> Words* {
        if (block.body.size() < 2) return nullptr;
        Words& in = block.body[block.body.size() - 2];
        const uint32_t opcode = in[0] & 0xffff;
        return opcode == op_loop_merge || opcode == op_selection_merge ? &in : nullptr;
    };
    const auto successors = [&](Block& block) {
        std::vector<uint32_t> out;
        const Words& t = block.body.back();
        switch (t[0] & 0xffff) {
        case op_branch: out.push_back(t[1]); break;
        case op_branch_conditional: out.push_back(t[2]); out.push_back(t[3]); break;
        case op_switch:
            out.push_back(t[2]);
            for (size_t k = 4; k < t.size(); k += 2) out.push_back(t[k]);
            break;
        }
        return out;
    };
    // Points a terminator's edges to one block at another.
    const auto retarget = [](Words& t, uint32_t from, uint32_t to) {
        switch (t[0] & 0xffff) {
        case op_branch: if (t[1] == from) t[1] = to; break;
        case op_branch_conditional:
            if (t[2] == from) t[2] = to;
            if (t[3] == from) t[3] = to;
            break;
        case op_switch:
            if (t[2] == from) t[2] = to;
            for (size_t k = 4; k < t.size(); k += 2)
                if (t[k] == from) t[k] = to;
            break;
        }
    };
    for (auto& block : blocks)
        if (block.body.empty() || !is_terminator(block.body.back()[0] & 0xffff))
            return refuse("a block without a terminator");

    size_t loops = 0, h = 0;
    for (size_t b = 0; b < blocks.size(); ++b)
        if (const Words* m = merge_of(blocks[b]); m && ((*m)[0] & 0xffff) == op_loop_merge) ++loops, h = b;
    bool has_switch = false;
    for (auto& block : blocks) has_switch |= (terminator(block)[0] & 0xffff) == op_switch;
    if (!has_switch) return std::vector<uint8_t>(code.begin(), code.end());
    if (loops != 1) return refuse("a switch without exactly one loop");
    Words* loop_merge = merge_of(blocks[h]);
    const uint32_t H = blocks[h].label, M = (*loop_merge)[1], C = (*loop_merge)[2];
    if ((terminator(blocks[h])[0] & 0xffff) != op_branch) return refuse("the loop header does not branch");
    const uint32_t D = terminator(blocks[h])[1];
    if (!index.contains(D) || !index.contains(M) || !index.contains(C)) return refuse("unknown loop blocks");
    Block& d_block = blocks[index[D]];
    Words* d_merge = merge_of(d_block);
    Words& d_switch = terminator(d_block);
    if (!d_merge || ((*d_merge)[0] & 0xffff) != op_selection_merge || (d_switch[0] & 0xffff) != op_switch ||
        d_block.body.size() != 2)
        return refuse("the loop header is not followed by a lone switch");
    const uint32_t S = (*d_merge)[1];
    if (d_switch[2] != S) return refuse("the switch's default is not its merge");
    if (!index.contains(S) || (terminator(blocks[index[S]])[0] & 0xffff) != op_branch ||
        terminator(blocks[index[S]])[1] != M)
        return refuse("the switch's merge does not leave the loop");
    if ((terminator(blocks[index[C]])[0] & 0xffff) != op_branch || terminator(blocks[index[C]])[1] != H)
        return refuse("the continue target does not branch back");
    if (merge_of(blocks[index[C]])) return refuse("the continue target is a header");

    // Types and constants.
    std::unordered_map<uint32_t, uint32_t> result_type;  // id -> type (globals and function)
    std::unordered_map<uint32_t, uint32_t> constant;      // 32-bit scalar constant id -> value
    std::unordered_map<uint32_t, const Words*> type_of;   // type id -> declaration
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> constant_ids;  // (type, value) -> id
    uint32_t bound = words[3], uint_type = 0, bool_type = 0;
    std::map<uint32_t, uint32_t> function_pointer;  // pointee -> Function pointer type
    for (const auto& in : globals) {
        const uint32_t opcode = in[0] & 0xffff;
        if (opcode >= op_type_bool && opcode <= 39 && in.size() >= 2) type_of[in[1]] = &in;
        if (opcode == op_type_int && in.size() >= 4 && in[2] == 32 && in[3] == 0) uint_type = in[1];
        if (opcode == op_type_bool) bool_type = in[1];
        if (opcode == op_type_pointer && in.size() >= 4 && in[2] == storage_function) function_pointer[in[3]] = in[1];
        if (opcode == op_constant && in.size() == 4) {
            constant[in[2]] = in[3];
            constant_ids.try_emplace({in[1], in[3]}, in[2]);
        }
        if ((opcode == op_constant || opcode == op_variable || (opcode >= 41 && opcode <= 46)) && in.size() >= 3)
            result_type[in[2]] = in[1];
    }
    if (!uint_type) return refuse("no 32-bit unsigned integer type");
    std::vector<Words> new_globals;
    const auto fresh = [&] { return bound++; };
    const auto uint_constant = [&](uint32_t value) {
        auto [it, added] = constant_ids.try_emplace({uint_type, value}, 0);
        if (added) {
            it->second = fresh();
            new_globals.push_back(make(op_constant, {uint_type, it->second, value}));
        }
        return it->second;
    };
    const auto pointer_to = [&](uint32_t type) {
        auto [it, added] = function_pointer.try_emplace(type, 0);
        if (added) {
            it->second = fresh();
            new_globals.push_back(make(op_type_pointer, {it->second, storage_function, type}));
        }
        return it->second;
    };
    if (!bool_type) {
        bool_type = fresh();
        new_globals.push_back(make(op_type_bool, {bool_type}));
    }

    // Every function instruction must be one this knows; note result types.
    std::unordered_map<uint32_t, uint32_t> defined_in;  // id -> block label
    std::vector<size_t> at;
    for (auto& block : blocks)
        for (auto& in : block.body) {
            if (!id_operands(in, at)) return refuse("instruction " + std::to_string(in[0] & 0xffff) + " in the function");
            if (has_result(in)) {
                result_type[in[2]] = in[1];
                defined_in[in[2]] = block.label;
            }
        }

    // The pc: the phi of H the switch reads. Its value entering the loop and
    // on each edge into the continue target must be a case literal.
    const uint32_t pc = d_switch[1];
    const Words* pc_phi = nullptr;
    for (const auto& in : blocks[h].body)
        if ((in[0] & 0xffff) == op_phi && in[2] == pc) pc_phi = &in;
    if (!pc_phi || pc_phi->size() != 7) return refuse("the switch does not read a two-way phi of the loop header");
    std::map<uint32_t, uint32_t> label_of;  // literal -> case label
    std::map<uint32_t, std::vector<uint32_t>> literals_of;  // label -> literals
    for (size_t k = 3; k + 1 < d_switch.size(); k += 2) {
        label_of[d_switch[k]] = d_switch[k + 1];
        literals_of[d_switch[k + 1]].push_back(d_switch[k]);
    }
    uint32_t preheader = 0, pc_entry = 0, pc_continue = 0;
    for (size_t k = 3; k + 1 < pc_phi->size(); k += 2) {
        if ((*pc_phi)[k + 1] == C) pc_continue = (*pc_phi)[k];
        else preheader = (*pc_phi)[k + 1], pc_entry = (*pc_phi)[k];
    }
    if (!preheader || !constant.contains(pc_entry) || !label_of.contains(constant[pc_entry]))
        return refuse("the loop is not entered at a case");
    if (!index.contains(preheader) || (terminator(blocks[index[preheader]])[0] & 0xffff) != op_branch)
        return refuse("the loop is not entered by a branch");
    std::unordered_map<uint32_t, uint32_t> continue_to;  // block branching to C -> literal it continues at
    if (constant.contains(pc_continue)) {
        for (auto& block : blocks)
            for (uint32_t s : successors(block))
                if (s == C) continue_to[block.label] = constant[pc_continue];
    } else {
        const Words* phi = nullptr;
        for (const auto& in : blocks[index[C]].body)
            if ((in[0] & 0xffff) == op_phi && in[2] == pc_continue) phi = &in;
        if (!phi) return refuse("the continued pc is not a phi of the continue target");
        for (size_t k = 3; k + 1 < phi->size(); k += 2) {
            if (!constant.contains((*phi)[k])) return refuse("a continue to a computed pc");
            continue_to[(*phi)[k + 1]] = constant[(*phi)[k]];
        }
    }
    for (const auto& [from, literal] : continue_to)
        if (!label_of.contains(literal)) return refuse("a continue to a pc without a case");
    constexpr uint32_t exit_pc = 0xffffffffu;
    if (label_of.contains(exit_pc)) return refuse("a case at the exit pc");

    // Pass A: phis become Function variables. A phi's value is stored on each
    // edge into its block (at the end of a predecessor with one successor,
    // else in a block on that edge); the phi becomes a load of it. Edges from
    // the switch D become each case's dispatch prologue, the default edge is
    // dead (every pc is a case), and the stores at the end of the continue
    // target run on every continue.
    std::vector<Words> variables;                        // OpVariable instructions for the entry block
    std::unordered_map<uint32_t, uint32_t> phi_variable;  // phi id -> its variable
    std::map<uint32_t, std::vector<Words>> prologue;      // case label -> stores on the edge from D
    std::unordered_map<uint32_t, std::vector<Words>> edge_stores_end;  // block -> stores before its terminator
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> split;           // (from, to) -> block on that edge
    std::vector<std::pair<size_t, Block>> inserted;                     // after block index
    for (auto& block : blocks)
        for (auto& in : block.body)
            if ((in[0] & 0xffff) == op_phi) {
                const auto type = type_of.find(in[1]);
                if (type == type_of.end() || ((*type->second)[0] & 0xffff) == op_type_pointer)
                    return refuse("a phi of a pointer or unknown type");
                const uint32_t variable = fresh();
                variables.push_back(make(op_variable, {pointer_to(in[1]), variable, storage_function}));
                phi_variable[in[2]] = variable;
            }
    for (size_t b = 0; b < blocks.size(); ++b) {
        Block& block = blocks[b];
        for (auto& in : block.body) {
            if ((in[0] & 0xffff) != op_phi) continue;
            const uint32_t variable = phi_variable[in[2]];
            for (size_t k = 3; k + 1 < in.size(); k += 2) {
                const uint32_t value = in[k], from = in[k + 1];
                Words store = make(op_store, {variable, value});
                if (from == D) {
                    if (block.label != S) prologue[block.label].push_back(std::move(store));
                    continue;
                }
                if (!index.contains(from)) return refuse("a phi from an unknown block");
                Block& pred = blocks[index[from]];
                auto succ = successors(pred);
                std::sort(succ.begin(), succ.end());
                succ.erase(std::unique(succ.begin(), succ.end()), succ.end());
                if (succ.size() == 1) {
                    edge_stores_end[from].push_back(std::move(store));
                    continue;
                }
                auto [it, added] = split.try_emplace({from, block.label}, 0);
                if (added) {
                    it->second = fresh();
                    inserted.push_back({index[from], Block{it->second, {make(op_branch, {block.label})}}});
                    if (from == D) return refuse("a split switch edge");
                }
                Block* edge = nullptr;
                for (auto& [after_index, e] : inserted)
                    if (e.label == it->second) edge = &e;
                edge->body.insert(edge->body.end() - 1, std::move(store));
            }
            in = make(op_load, {in[1], in[2], variable});
        }
    }
    // Phi loads go first in their blocks already (phis were first); stores
    // go before the merge and terminator.
    for (auto& [label, stores] : edge_stores_end) {
        Block& block = blocks[index[label]];
        const size_t tail = merge_of(block) ? 2 : 1;
        block.body.insert(block.body.end() - tail, stores.begin(), stores.end());
    }
    for (auto& [from_to, edge] : split) {
        retarget(terminator(blocks[index[from_to.first]]), from_to.second, edge);
        if (C == from_to.second) {
            if (!continue_to.contains(from_to.first)) return refuse("a continue edge without a pc");
            continue_to[edge] = continue_to[from_to.first];
        }
    }
    // Insert the edge blocks after their predecessors (keeping dominance order).
    {
        std::vector<Block> ordered;
        std::stable_sort(inserted.begin(), inserted.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        size_t next = 0;
        for (size_t b = 0; b < blocks.size(); ++b) {
            ordered.push_back(std::move(blocks[b]));
            while (next < inserted.size() && inserted[next].first == b) ordered.push_back(std::move(inserted[next++].second));
        }
        blocks = std::move(ordered);
        index.clear();
        for (size_t b = 0; b < blocks.size(); ++b) index[blocks[b].label] = b;
    }

    // H, D and C must hold nothing but their phi loads, stores and branches.
    const auto plain = [&](uint32_t label, bool stores) {
        for (const auto& in : blocks[index[label]].body) {
            const uint32_t opcode = in[0] & 0xffff;
            if (opcode == op_load && phi_variable.contains(in[2])) continue;
            if (stores && opcode == op_store) continue;
            if (opcode == op_loop_merge || opcode == op_selection_merge || is_terminator(opcode)) continue;
            return false;
        }
        return true;
    };
    if (!plain(H, false) || !plain(D, false) || !plain(C, true)) return refuse("work in the loop's own blocks");
    // The ids C defines (its phi loads) are read only in C.
    std::unordered_set<uint32_t> in_c;
    for (const auto& in : blocks[index[C]].body)
        if ((in[0] & 0xffff) == op_load) in_c.insert(in[2]);

    // The case regions: the blocks reached from each case label without
    // passing another case label, the loop's blocks or its exits.
    std::vector<uint32_t> labels;  // in literal order
    for (const auto& [literal, label] : label_of)
        if (std::find(labels.begin(), labels.end(), label) == labels.end()) labels.push_back(label);
    std::unordered_map<uint32_t, size_t> region_of_label;
    for (size_t r = 0; r < labels.size(); ++r) region_of_label[labels[r]] = r;
    // Order regions by their first literal (label_of is literal-ordered, so
    // labels already are); each label must have one literal.
    for (const auto& [label, list] : literals_of)
        if (list.size() != 1) return refuse("a case label with more than one literal");
    std::unordered_map<uint32_t, size_t> region;  // block -> region
    const std::unordered_set<uint32_t> stops = {H, D, C, S, M};
    for (size_t r = 0; r < labels.size(); ++r) {
        std::vector<uint32_t> stack{labels[r]};
        while (!stack.empty()) {
            const uint32_t label = stack.back();
            stack.pop_back();
            if (region.contains(label)) {
                if (region[label] != r) return refuse("a block shared by two cases");
                continue;
            }
            region[label] = r;
            Block& block = blocks[index[label]];
            std::vector<uint32_t> next = successors(block);
            if (const Words* m = merge_of(block)) {
                if (((*m)[0] & 0xffff) != op_selection_merge) return refuse("a loop inside a case");
                if (stops.contains((*m)[1]) || region_of_label.contains((*m)[1]))
                    return refuse("a selection in a case merging outside it");
                next.push_back((*m)[1]);
            }
            for (uint32_t s : next) {
                if (s == H || s == D || s == M) return refuse("a case branching to the loop's header or merge");
                if (s == C || s == S || region_of_label.contains(s)) continue;
                if (!index.contains(s)) return refuse("a branch to an unknown block");
                stack.push_back(s);
            }
        }
    }
    // The blocks after the loop: S, M and what M reaches.
    std::unordered_set<uint32_t> tail;
    {
        std::vector<uint32_t> stack{S};
        while (!stack.empty()) {
            const uint32_t label = stack.back();
            stack.pop_back();
            if (!tail.insert(label).second) continue;
            Block& block = blocks[index[label]];
            std::vector<uint32_t> next = successors(block);
            if (const Words* m = merge_of(block)) next.push_back((*m)[1]);
            for (uint32_t s : next) {
                if (region.contains(s) || s == H || s == C || s == D) return refuse("the loop's merge reaches the loop");
                stack.push_back(s);
            }
        }
    }
    // The blocks before the loop: the entry up to the preheader.
    std::unordered_set<uint32_t> head;
    for (auto& block : blocks) {
        const uint32_t label = block.label;
        if (region.contains(label) || tail.contains(label) || label == H || label == D || label == C) continue;
        head.insert(label);
    }
    for (uint32_t label : head)
        for (uint32_t s : successors(blocks[index[label]]))
            if (s != H && !head.contains(s)) return refuse("a block outside the loop branching into it");
    if (!head.contains(preheader)) return refuse("the preheader is inside the loop");

    // Transitions between regions: fallthroughs and continues. A backward one
    // needs a loop around the regions it spans.
    struct Exit { uint32_t block; uint32_t target; uint32_t literal; bool to_continue; };
    std::vector<Exit> exits;
    std::vector<std::pair<size_t, size_t>> spans;  // backward transitions: (to, from) regions
    for (auto& block : blocks) {
        if (!region.contains(block.label)) continue;
        const size_t r = region[block.label];
        std::vector<uint32_t> succ = successors(block);
        std::sort(succ.begin(), succ.end());
        succ.erase(std::unique(succ.begin(), succ.end()), succ.end());
        for (uint32_t s : succ) {
            if (s == C) {
                if (!continue_to.contains(block.label)) return refuse("a continue without a pc");
                const uint32_t literal = continue_to[block.label];
                exits.push_back({block.label, s, literal, true});
                const size_t to = region_of_label[label_of[literal]];
                if (to <= r) spans.push_back({to, r});
            } else if (s == S) {
                exits.push_back({block.label, s, exit_pc, false});
            } else if (region_of_label.contains(s) && region[s] != r) {
                exits.push_back({block.label, s, literals_of[s][0], false});
                const size_t to = region_of_label[s];
                if (to <= r) spans.push_back({to, r});
            } else if (region_of_label.contains(s)) {
                return refuse("a case branching back to its own label");
            }
        }
    }
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<size_t, size_t>> loops_of;
    for (const auto& [a, b] : spans) {
        if (!loops_of.empty() && a <= loops_of.back().second) loops_of.back().second = (std::max)(loops_of.back().second, b);
        else loops_of.push_back({a, b});
    }

    // Pass B: the new blocks.
    const uint32_t guard_variable = fresh();
    variables.push_back(make(op_variable, {pointer_to(uint_type), guard_variable, storage_function}));
    std::vector<uint32_t> guard(labels.size()), guard_merge(labels.size());
    for (size_t r = 0; r < labels.size(); ++r) guard[r] = fresh(), guard_merge[r] = fresh();
    std::map<size_t, std::pair<uint32_t, uint32_t>> loop_blocks;  // first region -> (header, continue)
    std::map<size_t, uint32_t> loop_merge_of;                     // first region -> merge
    for (const auto& [a, b] : loops_of) {
        loop_blocks[a] = {fresh(), fresh()};
        loop_merge_of[a] = fresh();
    }
    const auto entry_of = [&](size_t r) {
        return loop_blocks.contains(r) ? loop_blocks[r].first : guard[r];
    };
    const uint32_t after_last = S;

    // What runs on a transition to the case of a literal: through the old
    // dispatch (entering the loop, or a continue: then C's stores first) the
    // stores on the switch's edge to that case; then the guard variable. A
    // fallthrough or a break only sets the guard variable.
    const auto transition = [&](uint32_t literal, bool through_continue, bool dispatched) {
        std::vector<Words> code_out;
        if (through_continue) {
            std::unordered_map<uint32_t, uint32_t> renamed;
            for (const auto& in : blocks[index[C]].body) {
                const uint32_t opcode = in[0] & 0xffff;
                if (opcode == op_load) {
                    Words copy = in;
                    copy[2] = renamed[in[2]] = fresh();
                    code_out.push_back(std::move(copy));
                } else if (opcode == op_store) {
                    Words copy = in;
                    if (renamed.contains(copy[2])) copy[2] = renamed[copy[2]];
                    code_out.push_back(std::move(copy));
                }
            }
        }
        if (dispatched) {
            const auto& stores = prologue[label_of[literal]];
            code_out.insert(code_out.end(), stores.begin(), stores.end());
        }
        code_out.push_back(make(op_store, {guard_variable, uint_constant(literal)}));
        return code_out;
    };

    std::vector<Block> out_blocks;
    for (auto& block : blocks)
        if (head.contains(block.label)) {
            Block copy = block;
            if (copy.label == preheader) {
                auto code_in = transition(constant[pc_entry], false, true);
                copy.body.insert(copy.body.end() - 1, code_in.begin(), code_in.end());
                terminator(copy)[1] = entry_of(0);
            }
            out_blocks.push_back(std::move(copy));
        }
    std::vector<Block> exit_blocks;
    for (size_t r = 0; r < labels.size(); ++r) {
        size_t loop_first = SIZE_MAX, loop_last = SIZE_MAX;
        for (const auto& [a, b] : loops_of) {
            if (r >= a && r <= b) loop_first = a, loop_last = b;
        }
        if (loop_first == r) {
            const auto [header, cont] = loop_blocks[r];
            out_blocks.push_back({header, {make(op_loop_merge, {loop_merge_of[r], cont, 0}), make(op_branch, {guard[r]})}});
        }
        const uint32_t value = fresh();
        out_blocks.push_back({guard[r],
                              {make(op_load, {uint_type, value, guard_variable}),
                               make(op_selection_merge, {guard_merge[r], 0}),
                               make(op_switch, {value, guard_merge[r], literals_of[labels[r]][0], labels[r]})}});
        for (auto& block : blocks) {
            if (!region.contains(block.label) || region[block.label] != r) continue;
            Block copy = block;
            const uint32_t opcode = terminator(copy)[0] & 0xffff;
            for (const auto& e : exits) {
                if (e.block != block.label) continue;
                auto code_out = transition(e.literal, e.to_continue, e.to_continue);
                if (opcode == op_branch) {
                    copy.body.insert(copy.body.end() - (merge_of(copy) ? 2 : 1), code_out.begin(), code_out.end());
                    terminator(copy)[1] = guard_merge[r];
                } else if (opcode == op_branch_conditional || opcode == op_switch) {
                    const uint32_t edge = fresh();
                    code_out.push_back(make(op_branch, {guard_merge[r]}));
                    exit_blocks.push_back({edge, std::move(code_out)});
                    retarget(terminator(copy), e.target, edge);
                } else {
                    return refuse("an exit from a case by an unknown terminator");
                }
            }
            out_blocks.push_back(std::move(copy));
            for (auto& e : exit_blocks) out_blocks.push_back(std::move(e));
            exit_blocks.clear();
        }
        const uint32_t next = r + 1 < labels.size() ? entry_of(r + 1) : after_last;
        if (loop_last == r) {
            const auto [header, cont] = loop_blocks[loop_first];
            out_blocks.push_back({guard_merge[r], {make(op_branch, {cont})}});
            const uint32_t value = fresh(), low = fresh(), high = fresh(), inside = fresh();
            out_blocks.push_back(
                {cont,
                 {make(op_load, {uint_type, value, guard_variable}),
                  make(op_u_greater_than_equal, {bool_type, low, value, uint_constant(literals_of[labels[loop_first]][0])}),
                  make(op_u_less_than_equal, {bool_type, high, value, uint_constant(literals_of[labels[r]][0])}),
                  make(op_logical_and, {bool_type, inside, low, high}),
                  make(op_branch_conditional, {inside, header, loop_merge_of[loop_first]})}});
            out_blocks.push_back({loop_merge_of[loop_first], {make(op_branch, {next})}});
        } else {
            out_blocks.push_back({guard_merge[r], {make(op_branch, {next})}});
        }
    }
    for (auto& block : blocks)
        if (tail.contains(block.label)) out_blocks.push_back(block);
    blocks = std::move(out_blocks);
    index.clear();
    for (size_t b = 0; b < blocks.size(); ++b) index[blocks[b].label] = b;

    // Pass C: a value read outside the block that defines it goes through a
    // Function variable (stored after its definition, loaded before the use);
    // a phi's value is loaded from the phi's own variable; an access chain is
    // made again where it is used.
    defined_in.clear();
    std::unordered_map<uint32_t, Words> chains;     // access chain id -> its instruction
    std::unordered_set<uint32_t> local_variables;   // the function's own OpVariables
    for (auto& block : blocks)
        for (auto& in : block.body) {
            if (!has_result(in)) continue;
            defined_in[in[2]] = block.label;
            const uint32_t opcode = in[0] & 0xffff;
            if (opcode == op_access_chain || opcode == op_in_bounds_access_chain) chains[in[2]] = in;
            if (opcode == op_variable) local_variables.insert(in[2]);
        }
    std::unordered_map<uint32_t, uint32_t> value_variable;  // id -> its variable
    std::unordered_set<uint32_t> removed;  // ids whose definitions went with H, D and C
    for (const auto& [id, variable] : phi_variable)
        if (!defined_in.contains(id)) removed.insert(id);
    for (auto& block : blocks) {
        size_t i = 0;
        while (i < block.body.size()) {
            if (!id_operands(block.body[i], at)) return refuse("an unknown instruction after the rewrite");
            const uint32_t opcode = block.body[i][0] & 0xffff;
            bool again = false;
            for (size_t k : at) {
                if (opcode == op_variable) break;
                const uint32_t id = block.body[i][k];
                if (in_c.contains(id) && !defined_in.contains(id)) return refuse("a continue value read outside it");
                if (defined_in.contains(id) && defined_in[id] == block.label) continue;
                if (!defined_in.contains(id) && !phi_variable.contains(id)) continue;  // global or constant
                if (local_variables.contains(id)) continue;
                // Insert before the instruction, or before its merge.
                size_t before = i;
                if (is_terminator(opcode) && i > 0) {
                    const uint32_t previous = block.body[i - 1][0] & 0xffff;
                    if (previous == op_selection_merge || previous == op_loop_merge) before = i - 1;
                }
                const uint32_t type = result_type[id];
                const auto type_decl = type_of.find(type);
                if (type_decl == type_of.end()) return refuse("a value of an unknown type");
                if (((*type_decl->second)[0] & 0xffff) == op_type_pointer) {
                    // Make the access chain again here, then look at its operands.
                    if (!chains.contains(id)) return refuse("a pointer used outside its block");
                    Words copy = chains[id];
                    const uint32_t made = fresh();
                    copy[2] = made;
                    defined_in[made] = block.label;
                    chains[made] = copy;
                    block.body.insert(block.body.begin() + before, std::move(copy));
                    block.body[i + 1][k] = made;
                    i = before;
                    again = true;
                    break;
                }
                uint32_t variable;
                if (phi_variable.contains(id)) {
                    variable = phi_variable[id];
                } else {
                    auto [it, added] = value_variable.try_emplace(id, 0);
                    if (added) {
                        it->second = fresh();
                        variables.push_back(make(op_variable, {pointer_to(type), it->second, storage_function}));
                    }
                    variable = it->second;
                }
                const uint32_t loaded = fresh();
                defined_in[loaded] = block.label;
                block.body.insert(block.body.begin() + before, make(op_load, {type, loaded, variable}));
                ++i;
                block.body[i][k] = loaded;
            }
            if (!again) ++i;
        }
    }
    // Stores after the definitions of the values that got a variable.
    for (auto& block : blocks) {
        for (size_t i = 0; i < block.body.size(); ++i) {
            const Words& in = block.body[i];
            if (!has_result(in) || !value_variable.contains(in[2])) continue;
            const uint32_t variable = value_variable[in[2]], id = in[2];
            block.body.insert(block.body.begin() + i + 1, make(op_store, {variable, id}));
            ++i;
        }
    }
    // The variables go first in the entry block, after any it has.
    {
        auto& body = blocks.front().body;
        size_t first = 0;
        while (first < body.size() && (body[first][0] & 0xffff) == op_variable) ++first;
        body.insert(body.begin() + first, variables.begin(), variables.end());
    }

    // Write the module: globals (with the new types and constants before the
    // function), the function, the rest.
    std::vector<uint32_t> out(words.begin(), words.begin() + 5);
    out.reserve(words.size() * 2);
    for (const auto& in : globals) {
        const uint32_t opcode = in[0] & 0xffff;
        if ((opcode == op_name || opcode == op_decorate) && in.size() >= 2 && removed.contains(in[1])) continue;
        out.insert(out.end(), in.begin(), in.end());
    }
    for (const auto& in : new_globals) out.insert(out.end(), in.begin(), in.end());
    out.insert(out.end(), function_head.begin(), function_head.end());
    for (const auto& block : blocks) {
        out.push_back(2u << 16 | op_label);
        out.push_back(block.label);
        for (const auto& in : block.body) out.insert(out.end(), in.begin(), in.end());
    }
    out.push_back(1u << 16 | op_function_end);
    for (const auto& in : after) out.insert(out.end(), in.begin(), in.end());
    out[3] = bound;
    std::vector<uint8_t> bytes(out.size() * 4);
    std::memcpy(bytes.data(), out.data(), bytes.size());
    return bytes;
}

}  // namespace sfr
