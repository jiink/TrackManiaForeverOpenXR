# TrackMania Forever OpenXR

A VR mod for [TrackMania Nations Forever](https://store.steampowered.com/app/11020/TrackMania_Nations_Forever/) and [TrackMania United Forever](https://store.steampowered.com/app/7200/Trackmania_United_Forever/)

https://github.com/user-attachments/assets/5b76c802-7499-458f-a74e-979e9e26bcc3

> [!WARNING]
> ## AI made it.
> This repo's code is 100% LLM-generated. I didn't write any of this code, just the readme. All I did was sit with my VR headset on and launch the game whenever AI finished its turn.

BTW, **you have to disable fullscreen and anti aliasing in the Trackmania options, and the complete game window must fit within the monitor's usable area.** Current builds detect these incompatible conditions, show a warning, and continue on the desktop without initializing VR.

<img width="351" height="189" alt="image" src="https://github.com/user-attachments/assets/e39cf512-4586-4187-8dbd-8bc588ce553e" />

<img width="1064" height="490" alt="image" src="https://github.com/user-attachments/assets/83a78e11-3e29-4c07-8a37-f8dc205d3120" />

For more details, see [the AI generated readme](AI-GENERATED-README.md). I cannot vouch for its accuracy.

## My AI workflow for this project
 
People have been making VR mods using AI with some pretty good success. I wouldn't mind trying to do this myself, but the amount of time and effort it takes for the reward is poor: I don't care THAT much about being able to play xyz game in VR once and never again. All the good stuff like [Half-Life 2](https://store.steampowered.com/app/658920/HalfLife_2_VR_Mod/) and [Outer Wilds](https://outerwildsmods.com/mods/nomaivr/) has already been done by real people over many years of work.

Really I wanted to have a VR mod for Trackmania 2020, but got scared away by the likelihood of anti-cheat being in that game and being alerted by whatever's necessary to make a VR mod for it.

I googled to find if I could play any Trackmania in VR. I found that one medicore Trackmania game that I don't have has a VR mode, but that's it. I didn't see any mods for other TM games available.

Usually when I use AI help in coding it's just pasting code to chatbots in the browser like Google Gemini or ChatGPT. A few times I've tried using the agent feature built into VSCode, with mixed results. Last time I tried that - for a different project - I wrote up a detailed document about the software requirements and it just made so many stupid mistakes, ignored so many requirements, and got important math wrong after multiple tries. What a joke!

So for this, I wasn't sure what to use. I avoided Claude Code because it sounded like it had limited usage and high price. So I bought $5 of credits for DeepSeek and set up Aider (which lets your AI model of choice make code files and do git commits) since it's really cheap and I've heard it's pretty up to par. I gave it a detailed instructions file. This started off okay, but Aider stopped since DeepSeek somehow ended up disobeying the paritcular formatting Aider needs several times in a row. That was using the "reasoning" model. I tried to continue it with the faster "chat" model, but it quickly ended up spitting out the same two lines repeatedly indefinitley, something I haven't seen since, what, GPT-2? It also needed me to sit there and watch it for when it needed my approval to read files, make new files, etc. Aider's "auto approve" setting didn't make it so I could walk away while AI worked. Even when I did manually add files to the chat, the AI acted like I didn't. This was frustrating since I was looking for a hands-off approach. I had it work in a clone of the [ModTMNF](https://github.com/pixeltris/ModTMNF) repo since that seemed like a good starting point for hooking into the game. Then I realized that Trackmania Nations Forever is different from my target game, Trackmania United Forever. They might be similar enough games for that repo to be helpful for a VR mod, but I wasn't too sure, so I left that behind.

I decided I'd try the ChatGPT desktop app, which I thought was called Codex (but it's just called "ChatGPT"). I gave it a blank folder to work in, and just wrote a few sentences describing what I wanted (omitting my detailed instructions file from earlier), and told it where my Trackmania United Forever executable was located. I set it to auto-approve, set it to use the "Terra" model, I set "effort" to "high" (IDK how I'm supposed to know which model and effort combo I'm supposed to use.) After 13 minutes, without any of my intervention required, it had something for me to try, and it placed a new dll by the executable by itself. It's kinda scary how it can just do (mostly) whatever it wants on my PC, but it's either that, or me sitting there watching for 13 minutes and pressing "accept" once in a while. It started off just trying to get any image to show in the VR headset. from then on it was very incremental and it would be a couple hours before I saw anything stereoscopic. For a couple hours straight I would just have to put on my headset, launch the game, describe what I saw (which was often difficult), close the game, paste logs, take off my headset, and wait for the next turn. Soon I caved in and paid $20 for a month of premium and switched to their best model, "Sol", which I really hesitated to since I don't care a whole lot about seeing this game in VR.

After getting the game picture to show in the headset (in an extremely poor way), it started to make my head movement make the cursor move around, thinking this game had mouselook and thinking that mouselook was the right way to make a game work in VR. This was dissapointingly stupid and I saw it was trying to avoid the hard (but necessary) part of making the actual game camera move around in a custom way. I unfortunatley had to use my brain for a second and tell it that it was going the wrong approach. If I didn't interject here, I don't think this project would have gotten anywhere. I said that first we needed to see a stereo picture in the headset, and THEN it can make the openXR headset pose affect the game's camera pose.

That said, the ChatGPT app is the mostly-hands-off approach that I was looking for from the start, even though I really don't like spending $20 for the month rather than despositing $X of credits in.
