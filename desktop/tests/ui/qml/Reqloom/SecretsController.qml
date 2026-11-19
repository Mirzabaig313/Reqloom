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
            revealed: false
            revealedValue: ""
            revealMessage: ""
        }
        ListElement {
            name: "TOKEN"
            status: "set"
            detail: ""
            revealed: false
            revealedValue: ""
            revealMessage: ""
        }
    }

    property int reads: 0
    property bool hidden: false

    function refresh() {
    }

    // Like SecretsController: the keychain is read only when the person asks.
    function toggleReveal(name) {
        const row = secrets.count > 1 && secrets.get(1).name === name ? 1 : 0;
        if (secrets.get(row).revealed) {
            secrets.setProperty(row, "revealed", false);
            secrets.setProperty(row, "revealedValue", "");
            return;
        }
        reads += 1;
        secrets.setProperty(row, "revealed", true);
        secrets.setProperty(row, "revealedValue", "fake-stored-value");
    }

    function readStoredValue(name) {
        reads += 1;
        return {
            "shown": true,
            "value": "fake-stored-value",
            "message": ""
        };
    }

    function hideRevealed() {
        hidden = true;
        for (let i = 0; i < secrets.count; ++i) {
            secrets.setProperty(i, "revealed", false);
            secrets.setProperty(i, "revealedValue", "");
        }
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
