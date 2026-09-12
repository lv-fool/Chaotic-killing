import re
import collections

txt = open("dist/numeric_audit.log", encoding="utf-8").read()
lines = [l for l in txt.splitlines() if l.strip()]
pat = re.compile(r"op=(\w+)\s+ctx=\S+\s+reason=(\d)\s+roll=(\d+)\s+threshold=(\d+)\s+pass=(\d+)")

rolls = []
pass_n = 0
tot = 0
dmg = heal = 0
rhit = {0: [0, 0], 1: [0, 0], 2: [0, 0]}
for l in lines:
    m = pat.search(l)
    if not m:
        continue
    op = m.group(1)
    r = int(m.group(2))
    roll = int(m.group(3))
    thr = int(m.group(4))
    pas = int(m.group(5))
    tot += 1
    rolls.append(roll)
    pass_n += pas
    rhit[r][1] += 1
    rhit[r][0] += pas
    if op == "heal":
        heal += 1
    else:
        dmg += 1

print("判定总数:", tot, " damage:", dmg, " heal:", heal)
print("整体通过率: %.1f%%" % (100.0 * pass_n / tot))
for r in (0, 1, 2):
    n = rhit[r][1]
    if n:
        print("  reason=%d 通过率 %.1f%% (%d/%d)" % (r, 100.0 * rhit[r][0] / n, rhit[r][0], n))

buckets = collections.Counter((roll - 1) // 10 for roll in rolls)
print("数字十档分布(0为1-10区间):", dict(sorted(buckets.items())))
dups = {k: v for k, v in collections.Counter(rolls).items() if v > 5}
print("出现>5次的点数:", dups)
print("final=0(名义击杀)次数:", txt.count("final=0"))
ctxc = collections.Counter(re.findall(r"ctx=(\S+)", txt))
same = {k: v for k, v in ctxc.items() if v > 1}
print("相同判定上下文出现>1次的条目数:", len(same), " 单条最多出现次数:", max(same.values()))