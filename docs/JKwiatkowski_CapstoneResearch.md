# Capstone Research

**Jason Kwiatkowski**

**My project:** ELANORA is an EEG system that works in both directions. Forwards, it plays a sound pulsing at a certain frequency and measures what that does to your brain, heart and breathing. Backwards, you tell it the reaction you want and it predicts which frequency should get you there — or tells you that no frequency will.

---

## Resource 1 — BrainFlow Documentation and Source Code

**Organization:** BrainFlow development team (creator: Andrey Parfenov)
**Published:** Continuously maintained open-source project; version 5.16.0 used in this project
**Documentation:** https://brainflow.readthedocs.io
**Source code:** https://github.com/brainflow-dev/brainflow

BrainFlow is the library that actually talks to the Muse 2 headset, and its documentation lists what every supported headset sends, how fast it sends it, and how that data is grouped. That mattered immediately, because the Muse 2 isn't one stream — it's three running at different speeds: brain activity at 256 readings a second, head movement at 52, and heart rate at 64. The most useful thing I learned is that the heart rate sensor is off by default and only switches on if you send the headset a specific command once you've connected. The second most useful thing is that you should always ask the library which slot each sensor's data is in, instead of assuming, because if you guess wrong you don't get an error — you get numbers that look perfectly reasonable and are completely wrong. I built my whole device layer around asking instead of assuming, and when the website and the actual source code disagreed about a function, I went with the source code.

---

## Resource 2 — "Controlling the False Discovery Rate: A Practical and Powerful Approach to Multiple Testing"

**Authors:** Yoav Benjamini and Yosef Hochberg
**Published:** *Journal of the Royal Statistical Society, Series B (Methodological)*, Vol. 57, No. 1 (1995), pp. 289–300
**DOI:** 10.1111/j.2517-6161.1995.tb02031.x

This paper fixes a problem I would have walked straight into, because my system checks 22 things at once — four sensors times five brainwave bands, plus heart rate and breathing. The catch is that if you test 22 things and use the normal "5% chance this was luck" cutoff on each one, then roughly one of them will look like a real finding purely by luck, even when nothing is happening at all. Benjamini and Hochberg give a procedure for adjusting all 22 results together so that stops happening, and it's less harsh than the older method, so you don't throw away real findings to get there. I built that procedure into my statistics code, and it's now the gate everything has to pass: a result only counts if it clears the adjusted cutoff *and* beats a scrambled version of itself. That gate is the reason ELANORA can honestly say "nothing here" instead of always finding something.

---

## Resource 3 — *Gaussian Processes for Machine Learning*

**Authors:** Carl Edward Rasmussen and Christopher K. I. Williams
**Published:** MIT Press, 2006. ISBN 0-262-18253-X (ISBN-13: 978-0-262-18253-9)
**Free online:** https://gaussianprocess.org/gpml/

A Gaussian process is a kind of model that gives you two things instead of one: a prediction, and how confident it is in that prediction. That second part is the entire reason I picked it — when I ask which frequency should make someone more relaxed, I need the system to be suspicious about frequencies it barely has data for, not confident about something it has basically never seen. This book has both the math and the step-by-step algorithm, so I implemented it straight from the text. It also caught something I would have missed completely: past a certain distance from your real data the model's uncertainty stops growing, so if you wander far enough away it starts looking confident again. That's why my search is hard-locked to the frequencies I actually tested — being careful about uncertainty on its own would not have stopped it.

---

## How these three fit together

Each one covers a different stage of the project, and a different way it could go wrong. BrainFlow decides whether the data coming off the headset is real. Benjamini and Hochberg decide whether a result that looks real actually holds up. Rasmussen and Williams decide whether a recommendation can be trusted at a frequency I never directly tested.

Two of the three ended up making the system refuse to answer *more* often, not less. The statistics raised the bar for claiming an effect, and the uncertainty finding forced me to lock the search down to tested ground. That's the point of the project rather than a flaw in it — a system that always hands you a frequency is useless if frequency turns out not to do anything.
