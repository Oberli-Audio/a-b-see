# Failures - the product's own rows (D.9.3)

| Symptom | Cause | Confirming measurement | Fixed in | Regression test |
|---|---|---|---|---|
| Blind X/Y was not blind: X was the reference in every round, on every seed, so a blind comparison always ran the same way round and the tally would have been worthless | `juce::Random(seed).nextBool()` as the round's draw. A linear congruential generator's first output bit is correlated with the low bits of its seed; the bit `nextBool` takes came out the same for every small seed | `ABSeeHarness --seed=<1..12> --param=blind=1.0` -> `product.listen.x_is_mix` false for all twelve (2026-11-01) | 0.1.0, `Source/BlindRound.h`: splitmix64, which avalanches the seed by construction, one draw per round, still exactly reproducible (rule 8) | `test_blind_assignment_is_seeded` (shown failing on the build with `juce::Random`: "Items in the second set but not the first: True") |
