#include "entry.h"

void window_size_callback(GLFWwindow* window, int width, int height)
{
	gWidth = width;
	gHeight = height;
	on_size();
}

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	on_key(key, action);
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos)
{
	on_mouse(xpos, ypos);
}

auto run() -> int
{
	if (glfwInit() == GLFW_FALSE) return EXIT_FAILURE;

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	g_pWindow = glfwCreateWindow(800, 600, "Learn GL", nullptr, nullptr);
	if (g_pWindow == nullptr) return EXIT_FAILURE;

	glfwGetWindowSize(g_pWindow, &gWidth, &gHeight);

	init();
	auto prevWindowSizeCallback = glfwSetWindowSizeCallback(g_pWindow, window_size_callback);
	auto prevKeyCallback = glfwSetKeyCallback(g_pWindow, key_callback);
	auto prevMouseCallback = glfwSetCursorPosCallback(g_pWindow, mouse_callback);

	while (glfwWindowShouldClose(g_pWindow) == GLFW_FALSE)
	{
		update();
		draw();

		glfwPollEvents();
	}

	glfwSetKeyCallback(g_pWindow, prevKeyCallback);
	glfwSetWindowSizeCallback(g_pWindow, prevWindowSizeCallback);
	glfwSetCursorPosCallback(g_pWindow, prevMouseCallback);

	glfwDestroyWindow(g_pWindow);
	glfwTerminate();
	return EXIT_SUCCESS;
}
