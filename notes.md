I want to build a standalone viewer for geometry files like STEP, STL, etc. The top priorities for the viewer are for it to be as fast as possible and to use as little memory as possible. That is, I want to push hard on performance. We should be as lean as possible and appropriate.

We should choose the programming language that gives us the ability to get the most performance out. We can choose from Python, Rust, C++/C, etc. whichever works the best for us. Do a thorough, deep and critical analysis to push hard on performance.

I previously built a viewer in /Users/atharvaaalok/Desktop/Godela/exploration/claude/step_viewer which can serve as a reference for the aesthetics and behavior. We do not have to copy code, this viewer is just a reference for ideas. Our viewer should also not be tied to any specific library or previously stored dataset of geometry files. Users should be able to upload their own step or stl files. They should also be able to connect entire directories, in which case we show a directory tree with all the files that are available to be viewed inside the directory at different levels of sub-directories. This would allow users to connect libraries for example to view at once/together in one place. Again, we do not have to copy previous code, our focus should be on writing code that is extremely performant.

I want the following functionalities:
- mouse control
    - left mouse to select faces
    - middle mouse scroll for zoom
    - holding scroll button and moving would pan
    - holding h key and clicking a surface hides it, holding g and clicking a surface ghosts it (with shell, lines and both views available like on the proto godela platform: Desktop/Godela/platform).
    - I want a see through view like the one in Desktop/Godela/interesting_finds/colorfigures. I want to follow a color palette as the color figures theme.
    - I want the other color and rendering schemes available in step_viewer as well.
    - I want a settings menu where all these options on mouse control, special keys, rendering view etc. are available for the user to modify.
    - on hover I want surface highlight.
    - I want to be able to view the assembly hierarchy with entire assembly, sub-assemblies and parts if a step file defines it.
    - I want sectioning capabilities.
    - I want the rotation to use the adaptive rotation around a point method from proto godela at Desktop/Godela/platform.

These functionalities are also available in Desktop/Godela/main_platform and Desktop/Godela/platform.

Write the highest quality code in terms of maintainence. Make small and regular atomic commits and follow best coding practice.

I want to make the viewer aesthetically pleasing and very well designed. Pay great attention to aesthetic and visual design. We should build the best, highest quality and performant viewer which can work as a standalone project. The above examples from the previous step viewer, proto godela and the main platform were just for reference and inspiration, we do not have to copy the code. We should develop the viewer from scratch for the best user experience.

Again, I emphasize that our main objective is extreme performance in terms of speed and memory. Wherever there is a choice between them, prioritize speed.

Dispatch several sub-agents to perform a deep dive, get a thorough understanding and analyze critically.