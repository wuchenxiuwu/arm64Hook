# arm64hook
--------------
This is an arm64Hook dynamic link library. You need to compile it. For specific details, please refer to the code or the calling method with the attached header file
---------------
In the Termux environment, compile as a dynamic link library as follows
---------------
g++ -std=c++20 -fPIC -pthread -Wall -O2 -c arm64Hook.cpp -o arm64Hook.o
---------------
g++ -shared -pthread arm64Hook.o -o libarm64hook.so
---------------
The specific details can be adjusted according to your own needs
---------------
