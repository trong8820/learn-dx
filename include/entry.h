#ifndef ENTRY_H__
#define ENTRY_H__

#include <iostream>
#include <string>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

GLFWwindow* g_pWindow;

int gWidth{ 0 };
int gHeight{ 0 };

int run();

bool init();
void update();
void draw();

void on_size();
void on_key(int key, int action);
void on_mouse(double xpos, double ypos);

#endif // ENTRY_H__

