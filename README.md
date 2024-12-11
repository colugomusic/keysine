This is a single-file C++20 audio application written for educational purposes which turns your computer keyboard into an extremely simple sine wave synthesizer.

The code has been deliberately over-engineered to show what I think are many useful techniques for architecting very large and complex audio applications such as digital audio workstations, based on what I have learned so far working on Blockhead.

Note that this is not a "very large and complex audio application", it is just a very small toy example, and therefore the techniques being used here are completely overkill for the end result. The only point of the project is to try to show off some fairly advanced techniques in as few lines of code as possible.

Libraries used:

- [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake): CMake package management
- [bhas](https://github.com/colugomusic/bhas): Just for starting and stopping audio streams.
- [immer](https://github.com/arximboldi/immer): For representing project state as an immutable data structure.
- [ez](https://github.com/colugomusic/ez): For synchronizing project state with the audio thread.
- [readerwriterqueue](https://github.com/cameron314/readerwriterqueue): For one-way communication from the audio thread back to the main thread.
- [SDL2](https://github.com/libsdl-org/SDL): Only for handling keyboard input.
