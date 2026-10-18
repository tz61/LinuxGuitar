// Automatic fingering: Viterbi search over notes without an assigned string; the cost favors beginners
// (low positions, open strings, few position shifts, small finger stretches).
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>

#include "song/SongInternal.h"

namespace gs {

namespace {

constexpr int kMaxFret = 20;
constexpr int kMaxCand = 24;

struct Cand {
    std::array<int8_t, 6> str{};   // string assigned to each pending note (-1 = drop this note)
    int pos = -1;                  // hand position: lowest fretted fret, -1 = no fretted notes
    float local = 0.f;
};

struct EvCtx {
    size_t ev = 0;
    std::vector<int> freeIdx;      // indices of the event's notes with string == -1
    std::vector<Cand> cands;
    std::vector<int> back;         // predecessor of each candidate
    std::vector<float> cost;
};

float localCost(const int* frets, int n) {
    int mn = 99, mx = -1, fretted = 0;
    float c = 0.f;
    for (int i = 0; i < n; ++i) {
        int f = frets[i];
        if (f < 0) continue;
        if (f == 0) { c += 0.05f; continue; }
        ++fretted;
        c += 0.3f * (float)f + (f > 5 ? 0.7f * (float)(f - 5) : 0.f) + (f > 12 ? 1.0f * (float)(f - 12) : 0.f);
        mn = std::min(mn, f);
        mx = std::max(mx, f);
    }
    if (fretted >= 2) {
        int span = mx - mn;
        c += 0.3f * (float)span;
        if (span > 3) c += 4.f * (float)(span - 3);
    }
    if (fretted > 4) c += 3.f * (float)(fretted - 4);  // only 4 fingers (barres are not modeled here)
    return c;
}

float transCost(int a, int b) {
    if (a < 0 || b < 0) return 0.f;
    int d = std::abs(a - b);
    return 0.8f * (float)d + (d > 4 ? 1.5f * (float)(d - 4) : 0.f);
}

// move notes outside the guitar's range into it by octaves
int foldIntoRange(int midi, const Song& s) {
    int lo = s.tuning[5] + s.capo, hi = s.tuning[0] + s.capo + kMaxFret;
    for (int k = 0; k < 10 && midi < lo; ++k) midi += 12;
    for (int k = 0; k < 10 && midi > hi; ++k) midi -= 12;
    return midi;
}

void genCands(const Song& song, const SongEvent& e, EvCtx& ctx) {
    ctx.cands.clear();
    bool used[6] = {false, false, false, false, false, false};
    int fixedFrets[6];
    int nFixed = 0;
    for (const SongNote& n : e.notes)
        if (n.string >= 0 && n.string < 6) { used[n.string] = true; fixedFrets[nFixed++] = n.fret; }
    const int nf = (int)ctx.freeIdx.size();
    // assign high notes first (melody notes matter most)
    std::vector<int> order(nf);
    for (int i = 0; i < nf; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return e.notes[ctx.freeIdx[a]].midi > e.notes[ctx.freeIdx[b]].midi; });

    std::vector<Cand> all;
    Cand cur;
    cur.str.fill(-1);
    std::function<void(int)> dfs;
    int frets[12];
    dfs = [&](int depth) {
        if ((int)all.size() >= 4096) return;
        if (depth == nf) {
            int nfr = 0;
            for (int i = 0; i < nFixed; ++i) frets[nfr++] = fixedFrets[i];
            int dropped = 0;
            int mn = 99;
            for (int i = 0; i < nf; ++i) {
                int s = cur.str[i];
                if (s < 0) { ++dropped; continue; }
                int f = e.notes[ctx.freeIdx[i]].midi - song.tuning[s] - song.capo;
                frets[nfr++] = f;
                if (f > 0) mn = std::min(mn, f);
            }
            for (int i = 0; i < nFixed; ++i)
                if (fixedFrets[i] > 0) mn = std::min(mn, fixedFrets[i]);
            Cand c = cur;
            c.pos = mn == 99 ? -1 : mn;
            c.local = localCost(frets, nfr) + 50.f * (float)dropped;
            all.push_back(c);
            return;
        }
        int ni = order[depth];
        int midi = e.notes[ctx.freeIdx[ni]].midi;
        bool any = false;
        for (int s = 0; s < 6; ++s) {
            if (used[s]) continue;
            int f = midi - song.tuning[s] - song.capo;
            if (f < 0 || f > kMaxFret) continue;
            any = true;
            used[s] = true;
            cur.str[ni] = (int8_t)s;
            dfs(depth + 1);
            used[s] = false;
            cur.str[ni] = -1;
        }
        if (!any) dfs(depth + 1);  // not enough strings or nowhere to put it: drop the note (high cost)
    };
    dfs(0);
    if (all.empty()) {
        Cand c;
        c.str.fill(-1);
        c.local = 50.f * (float)nf;
        all.push_back(c);
    }
    std::sort(all.begin(), all.end(), [](const Cand& a, const Cand& b) { return a.local < b.local; });
    // keep the cheapest candidates, with at least one per hand position
    std::vector<Cand> keep;
    std::array<bool, kMaxFret + 2> posSeen{};
    for (const Cand& c : all) {
        int pk = c.pos < 0 ? kMaxFret + 1 : c.pos;
        if ((int)keep.size() < kMaxCand / 2 || (!posSeen[pk] && (int)keep.size() < kMaxCand)) {
            keep.push_back(c);
            posSeen[pk] = true;
        }
        if ((int)keep.size() >= kMaxCand) break;
    }
    ctx.cands.swap(keep);
}

}  // namespace

void autoFinger(Song& song) {
    // first fold notes outside the range into it
    std::vector<size_t> evs;
    for (size_t i = 0; i < song.events.size(); ++i) {
        bool need = false;
        for (SongNote& n : song.events[i].notes)
            if (n.string < 0) {
                if (n.midi >= 0) n.midi = foldIntoRange(n.midi, song);
                need = true;
            }
        if (need) evs.push_back(i);
    }
    if (evs.empty()) return;

    // dedupe repeated pitches within an event (after folding); there cannot be more pending notes than free
    // strings (keep the lowest note + the highest few)
    for (size_t idx : evs) {
        auto& notes = song.events[idx].notes;
        std::vector<SongNote> fixed, freeN;
        for (const SongNote& n : notes) {
            if (n.string >= 0) { fixed.push_back(n); continue; }
            bool dup = false;
            for (const SongNote& k : freeN)
                if (k.midi == n.midi) dup = true;
            if (!dup) freeN.push_back(n);
        }
        int avail = 6 - std::min<int>(6, (int)fixed.size());
        if ((int)freeN.size() > avail) {
            std::sort(freeN.begin(), freeN.end(), [](const SongNote& a, const SongNote& b) { return a.midi > b.midi; });
            if (avail <= 0) {
                freeN.clear();
            } else {
                SongNote lowest = freeN.back();
                freeN.resize((size_t)avail);
                freeN.back() = lowest;
            }
        }
        notes = fixed;
        notes.insert(notes.end(), freeN.begin(), freeN.end());
    }

    // chunked Viterbi (at most 2000 events per chunk, chained through the previous chunk's hand position)
    int carryPos = -1;
    const size_t kChunk = 2000;
    for (size_t c0 = 0; c0 < evs.size(); c0 += kChunk) {
        size_t c1 = std::min(evs.size(), c0 + kChunk);
        std::vector<EvCtx> ctx(c1 - c0);
        for (size_t k = c0; k < c1; ++k) {
            EvCtx& x = ctx[k - c0];
            x.ev = evs[k];
            const SongEvent& e = song.events[x.ev];
            for (size_t ni = 0; ni < e.notes.size(); ++ni)
                if (e.notes[ni].string < 0) x.freeIdx.push_back((int)ni);
            genCands(song, e, x);
            x.cost.assign(x.cands.size(), std::numeric_limits<float>::infinity());
            x.back.assign(x.cands.size(), -1);
        }
        for (size_t j = 0; j < ctx[0].cands.size(); ++j) ctx[0].cost[j] = ctx[0].cands[j].local + transCost(carryPos, ctx[0].cands[j].pos);
        for (size_t k = 1; k < ctx.size(); ++k) {
            EvCtx& prev = ctx[k - 1];
            EvCtx& cur = ctx[k];
            // halve the shift cost between events far apart (more than 4 beats)
            double gap = song.events[cur.ev].beat - song.events[prev.ev].beat;
            float w = gap > 4.0 ? 0.5f : 1.f;
            for (size_t j = 0; j < cur.cands.size(); ++j) {
                float best = std::numeric_limits<float>::infinity();
                int bi = 0;
                for (size_t i = 0; i < prev.cands.size(); ++i) {
                    int pp = prev.cands[i].pos;
                    float t = prev.cost[i] + w * transCost(pp, cur.cands[j].pos);
                    if (t < best) { best = t; bi = (int)i; }
                }
                cur.cost[j] = best + cur.cands[j].local;
                cur.back[j] = bi;
            }
        }
        // backtrack
        EvCtx& last = ctx.back();
        int bj = (int)(std::min_element(last.cost.begin(), last.cost.end()) - last.cost.begin());
        for (size_t k = ctx.size(); k-- > 0;) {
            EvCtx& x = ctx[k];
            const Cand& c = x.cands[bj];
            SongEvent& e = song.events[x.ev];
            for (size_t i = 0; i < x.freeIdx.size(); ++i) {
                SongNote& n = e.notes[x.freeIdx[i]];
                int s = c.str[i];
                if (s >= 0) {
                    n.string = s;
                    n.fret = n.midi - song.tuning[s] - song.capo;
                }
            }
            if (k == ctx.size() - 1) carryPos = c.pos;
            bj = x.back[bj] < 0 ? 0 : x.back[bj];
        }
    }
}

}  // namespace gs
