This is an OpenAL-soft shared library for Windows Store App/Windows Phone 8.0+.

Features:
 * back-end for Windows Audio Session API (WASAPI)

To build, open Visual Studio solution in winrt.vs2012 folder. 
 
This is a fork of OpenAL-soft 

Important Notes:
 * For Windows Store App, alcOpenDevice() is not allowed to be called on UI thread. 
 All OpenAL function are not allowed to be called before UI window is ready/visible.
