#include <iostream>
#include <vector>

struct DX7AlgorithmDef {
    int  modulatedBy[4]; // bit j set => op j's output modulates op i
    bool isCarrier[4];
};

static constexpr DX7AlgorithmDef kDX7Algorithms[32] = {
    //  Alg 1:  3->2->1->0(C)  [linear chain]
    { {1<<1, 1<<2, 1<<3, 0}, {true,false,false,false} },
    //  Alg 2:  3->2->1->0(C), 3->0(C)  [chain + shortcut to carrier]
    { {1<<1|1<<3, 1<<2, 1<<3, 0}, {true,false,false,false} },
    //  Alg 3:  3->2->0(C), 3->1->0(C)  [branching mod feeds two paths]
    { {1<<1|1<<2, 1<<3, 1<<3, 0}, {true,false,false,false} },
    //  Alg 4:  3->2->0(C), 1->0(C)  [two independent branches into carrier]
    { {1<<1|1<<2, 0, 1<<3, 0}, {true,false,false,false} },
    //  Alg 5:  3->2->0(C), 3->1(C)  [shared mod, dual carriers]
    { {1<<2, 1<<3, 1<<3, 0}, {true,true,false,false} },
    //  Alg 6:  3->2->0(C), 1(C)  [chain + independent bare carrier]
    { {1<<2, 0, 1<<3, 0}, {true,true,false,false} },
    //  Alg 7:  2->1->0(C), 3->0(C)  [chain-of-3 + direct extra mod to carrier]
    { {1<<1|1<<3, 1<<2, 0, 0}, {true,false,false,false} },
    //  Alg 8:  2->1->0(C), 3->1  [extra mod feeds middle of chain]
    { {1<<1, 1<<2|1<<3, 0, 0}, {true,false,false,false} },
    //  Alg 9:  3->1->0(C), 2->0(C)  [two separate chains into carrier]
    { {1<<1|1<<2, 1<<3, 0, 0}, {true,false,false,false} },
    //  Alg 10: 3->2->1->0(C), 2->0(C)  [chain with mid-point bypass to carrier]
    { {1<<1|1<<2, 1<<2, 1<<3, 0}, {true,false,false,false} },
    //  Alg 11: 2->1->0(C), 3->1  [Y: two mods fan into op1 which feeds carrier]
    { {1<<1, 1<<2|1<<3, 0, 0}, {true,false,false,false} },
    //  Alg 12: 2->1->0(C), 3(C)  [chain of 3 + bare fourth carrier]
    { {1<<1, 1<<2, 0, 0}, {true,false,false,true} },
    //  Alg 13: 2->1(C), 2->0(C), 3(C)  [one mod fans to two carriers + bare]
    { {1<<2, 1<<2, 0, 0}, {true,true,false,true} },
    //  Alg 14: 3->2->1(C), 3->0(C)  [chain exits mid-way + shortcut to op0]
    { {1<<3, 1<<2, 1<<3, 0}, {true,true,false,false} },
    //  Alg 15: 1->0(C), 3->2(C)  [two independent 2-op stacks]
    { {1<<1, 0, 1<<3, 0}, {true,false,true,false} },
    //  Alg 16: 1->0(C), 2(C), 3(C)  [one 2-op stack + two bare carriers]
    { {1<<1, 0, 0, 0}, {true,false,true,true} },
    //  Alg 17: 3->0(C), 2->1(C)  [two crossed 2-op stacks, different pairing]
    { {1<<3, 0, 1<<2, 0}, {true,true,false,false} },
    //  Alg 18: 3->0(C), 2->0(C), 1->0(C)  [star: all mods into single carrier]
    { {1<<1|1<<2|1<<3, 0, 0, 0}, {true,false,false,false} },
    //  Alg 19: 3->0(C), 2->0(C), 1(C)  [two mods into carrier + bare carrier]
    { {1<<2|1<<3, 0, 0, 0}, {true,true,false,false} },
    //  Alg 20: 3->2(C), 1->0(C)  [two independent 2-op stacks, ops swapped]
    { {1<<1, 0, 1<<3, 0}, {true,false,true,false} },
    //  Alg 21: 3->2(C), 3->1(C), 3->0(C)  [one mod fans to three carriers]
    { {1<<3, 1<<3, 1<<3, 0}, {true,true,true,false} },
    //  Alg 22: 3->2(C), 1(C), 0(C)  [one 2-op stack + two bare carriers]
    { {0, 0, 1<<3, 0}, {true,true,true,false} },
    //  Alg 23: 3->2(C), 3->1(C), 0(C)  [branching mod + independent bare carrier]
    { {0, 1<<3, 1<<3, 0}, {true,true,true,false} },
    //  Alg 24: 0(C), 1(C), 2(C), 3(C)  [all carriers — pure additive synthesis]
    { {0, 0, 0, 0}, {true,true,true,true} },
    //  Alg 25: 3->2->1(C), 0(C)  [chain of 3 exiting at op1 + bare op0 carrier]
    { {0, 1<<2, 1<<3, 0}, {true,true,false,false} },
    //  Alg 26: 3->1(C), 2->0(C)  [cross-paired 2-op stacks]
    { {1<<2, 1<<3, 0, 0}, {true,true,false,false} },
    //  Alg 27: 3->2->1(C), 2->0(C)  [chain forks: op2 also drives carrier op0]
    { {1<<2, 1<<2, 1<<3, 0}, {true,true,false,false} },
    //  Alg 28: 3->2->1->0(C), 3->1  [chain + shortcut to mid-point op1]
    { {1<<1, 1<<2, 1<<3|1<<2, 0}, {true,false,false,false} },
    //  Alg 29: 3->0(C), 2->1->0(C)  [direct mod + 2-deep chain both into carrier]
    { {1<<1|1<<3, 1<<2, 0, 0}, {true,false,false,false} },
    //  Alg 30: 3->1->0(C), 2->1  [extra mod feeds chain entry op1]
    { {1<<1, 1<<2|1<<3, 0, 0}, {true,false,false,false} },
    //  Alg 31: 3->2->0(C), 2->1->0(C)  [op2 shared: two paths converge on carrier]
    { {1<<1|1<<2, 1<<2, 1<<3, 0}, {true,false,false,false} },
    //  Alg 32: 3->0(C), 1->0(C), 2->1  [op2 feeds op1 feeds carrier, op3 direct too]
    { {1<<1|1<<3, 1<<2, 0, 0}, {true,false,false,false} },
};

int main() {
    for (int i = 0; i < 32; ++i) {
        for (int op = 0; op < 4; ++op) {
            if (kDX7Algorithms[i].modulatedBy[op] & (1 << op)) {
                std::cout << "Alg " << (i+1) << " has self feedback on op " << op << "\n";
            }
        }
    }
    return 0;
}
