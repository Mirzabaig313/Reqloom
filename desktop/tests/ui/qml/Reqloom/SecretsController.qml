// Test stub of the SecretsController singleton: one referenced secret, a store
// that refuses an empty value the way SecretManager does, and a record of the
// folder each move confirmation named.
pragma Singleton
import QtQuick

QtObject {
    property bool backendAvailable: true
    property string folderConflict: ""
    property string currentFolder: "/projects/here"
    property string lastError: ""
    property bool failStore: false
    property int storedCount: 0
    property string confirmedOwner: ""
    property ListModel secrets: ListModel {
        ListElement {
            name: "API_KEY"
            status: "not set"
            detail: ""
        }
    }

    function refresh() {
    }

    function store(name, value) {
        if (value.length === 0) {
            lastError = "Failed to set secret: Enter a value.";
            return false;
        }
        if (failStore) {
            lastError = "Failed to set secret: keychain locked";
            return false;
        }
        storedCount += 1;
        lastError = "";
        return true;
    }

    function clear(name) {
        return true;
    }

    function confirmMove(shownOwner) {
        confirmedOwner = shownOwner;
        lastError = "Couldn't use the secrets here: refused";
        return false;
    }
}
