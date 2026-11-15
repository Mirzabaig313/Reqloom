// Test stub of the AppController singleton: fixed suggestions and candidate
// values, and a record of every pinned override so tests can assert none slipped in.
pragma Singleton
import QtQuick

QtObject {
    property bool running: false
    property var pins: []
    property var overrides: ({})

    signal variableOverridesChanged

    function variableSuggestions(operationId) {
        return [
            {
                "token": "users.id",
                "kind": "extract"
            },
            {
                "token": "abc.id",
                "kind": "extract"
            },
            {
                "token": "tab",
                "kind": "env"
            }
        ];
    }

    function candidateValues(token) {
        return ["101", "102"];
    }

    function setVariableOverride(token, value) {
        pins = pins.concat([token + "=" + value]);
        overrides[token] = value;
        variableOverridesChanged();
    }

    function variableOverride(token) {
        return overrides[token] || "";
    }

    function producerOpFor(token) {
        return "users.list";
    }

    function refreshCandidates(token) {
    }

    function suggestExtractionPaths(operationId, text) {
        return ["data.items", "data.id", "id"];
    }
}
