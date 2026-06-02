
1. analyze and describe in a claude.md the contents of the v1.25 folder containing the 8086 assemly implementation of msdos 1.25
2. create a reference document of 8086 assembly that can later be used to port this project to c code
3. port the operating system to c code. for keyboard input and screen output write a wrapper cli application in c. the wrapper has to support disk reads / disk writes to an in memory dist that is dumped and can be loaded when the wrapper opens and closes.
4. create a help command with all commands supported in this dos version
5. add a MKFILE command that creates an empty file with a given file name in the current directory
6. create an open command that opens a file and shows its contents as ascii
7. add a readme.md that describes the project and attributes the https://github.com/microsoft/ms-dos repo for the original msdos versions
8. add an EDIT command that allows the user to view and edit and save a text file on disk. change the wrapper to allow for this if required
9. change the moving the arrows closes the editor, ^Q command does not work on keyboard
10. what control keys are the commands in the editor supposed to listen to? for example ^Q 
11. rename the MKFILE to CREATE command (also update readme.md and HELP reference)
12. add MKDIR and RMDIR and CD commands commands to create, delete and navigate to folders
13. update readme and claude.md with this change
14. add a rudimentary version of basic where the command BASIC can be used to run a given file

