Generic LV2 Plugin and PluginControl Classes
Do not add any audio backend specific code in these classes. Focus on the generic implementation that can be used with any audio backend.

Create new file with following classes:

1. PluginControl Class:
- Support different types of plugin controls based on type of control specified in the LV2 plugin descriptor.
- Types of controls to support:
  - LV2ControlPort
    - float
    - Toggle
    - Trigger
  - LV2AtomPort
    - Atom
    - AtomSequence
- Implement methods to set and get control values, ensuring type safety and proper handling of each control type.
- Include error handling for unsupported control types and invalid value assignments.
- Minimum / Maximum / Default value handling for float controls.
- On initialization, read the control values from the LV2 plugin descriptor and set up the controls accordingly.
- Provide a method to update the control values based on user input or changes in the plugin state
- Implement a method to reset controls to their default values.
- Get and set methods for each control type, ensuring that the correct data types are used and that values are within valid ranges where applicable.

2. LV2Plugin Class:
- Implement a generic LV2Plugin class that can load and manage any LV2 plugin based on its descriptor.
- The class should be able to:
  - Load the plugin descriptor and initialize the plugin instance.
  - Manage the plugin's ports and controls using the PluginControl class.
  - Handle audio processing by connecting the plugin's ports to the appropriate buffers.
  - Provide methods to start and stop the plugin, as well as to process audio data in real-time.
  - Implement error handling for plugin loading failures, unsupported features, and processing errors.
- Include functionality to save and load plugin state, allowing users to preserve their settings across sessions.
- Provide a user interface for interacting with the plugin controls, allowing users to adjust parameters and see real-time feedback on their changes.
- Ensure that the class is designed to be extensible, allowing for future enhancements and support for additional control types or features as needed.
- Provide the following interface:
  - `loadPlugin(const std::string& pluginPath)`: Load the LV2 plugin from the specified path.
  - `initialize()`: Initialize the plugin instance and set up controls based on the descriptor.
  - `start()`: Start the plugin for audio processing.
  - `stop()`: Stop the plugin and release resources.
  - `process(float* inputBuffer, float* outputBuffer, int numFrames)`: Process audio data in real-time. Also process atom messages to and from the plugin, handling control changes and state updates. This will be called from an audio callback, so it must be efficient and thread-safe.
  - `setControlValue(const std::string& controlName, const std::variant<float, bool, std::vector<uint8_t>>& value)`: Set the value of a control by name.
  - `getControlValue(const std::string& controlName)`: Get the current value of a control by name.
  - `saveState(const std::string& filePath)`: Save the current state of the plugin to a file.
  - `loadState(const std::string& filePath)`: Load a saved state from a file and apply it to the plugin.