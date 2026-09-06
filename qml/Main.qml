import QtQuick 1.0
import "dialog"
import "message"
import "control"
import "auth"
import SymboGram 1.0

Item {
    //TODO: remove dynamically unused pages / components from memory
    //TODO: keypad navigation
    id: root

    width: 320
    height: 240

    property color globalAccent: platformUtils.isWindows() ? platformUtils.windowsRealColorizationColor() : "#54759E"

    state: "AUTH"

    states: [
        State {
            name: "AUTH"
            PropertyChanges {
                target: mainScreen
                anchors.leftMargin: -root.width
                opacity: 0
            }
            PropertyChanges {
                target: authScreen
                anchors.leftMargin: 0
            }
        },
        State {
            name: "MAIN"
            PropertyChanges {
                target: mainScreen
                anchors.leftMargin: 0
                opacity: 1
            }
            PropertyChanges {
                target: authScreen
                anchors.leftMargin: -root.width
            }
        }
    ]

    transitions: [
        Transition {
            NumberAnimation {
                properties: "anchors.leftMargin,opacity"
                easing.type: Easing.InOutQuad
                duration: 200
            }
        }
    ]

    property int currentFolderIndex: 0
    property bool authProgress: false

    function setAuthProgress(visible) {
        authProgress = visible;
    }

    Component.onCompleted: {
        if (telegramClient.hasSession()) {
            setAuthProgress(true);
            telegramClient.start();
        }
    }

    TgClient {
        id: telegramClient

        onInitialized: {
            if (hasUserId) {
                return;
            }

            setAuthProgress(false);
            authScreen.currentIndex = 1;

            helpGetCountriesList();
        }

        onDisconnected: {
            setAuthProgress(false);

            //Not while the password page is up: the key derivation is the
            //longest window in the login flow for a blip to land in, and
            //dropping to the intro page discards a password mid-check.
            if (!hasUserId && authScreen.currentIndex != 3) {
                root.state = "AUTH";
                authScreen.currentIndex = 0;
            }

            // No start() here. The transport schedules its own retry with a
            // capped backoff; calling start() from this handler reconnects at
            // the speed of the event loop, because a refused connection emits
            // disconnected again immediately.
        }

        onReconnecting: {
            snackBar.text = delayMs < 2000
                    ? "Reconnecting..."
                    : "Reconnecting in " + Math.round(delayMs / 1000) + "s...";
        }

        onAuthSentCodeResponse: {
            setAuthProgress(false);

            //auth.sentCodePaymentRequired, new at layer 229. Telegram wants a
            //paid product bought before it will send a code, and this
            //constructor carries no "type" at all -- so the switch below reads
            //undefined and we would advance to a code page for a code that is
            //never coming. There is no in-app purchase here and there will not
            //be one, so say so and stay put.
            //Signed decimal, not 0xf8827ebf: the id is negative as an int32 and
            //the hex literal would compare as a large positive in QML.
            if (data["_"] == -125665601) {
                snackBar.text = "Telegram is asking for a paid product before it will send a login code. Please use the official app.";
                return;
            }

            authScreen.phonePage.phoneCodeHash = data["phone_code_hash"];
            switch (data["type"]["_"]) {
                //TODO messages
//            case TLType::AuthSentCodeTypeApp:
//                codeNumberDescriptionLabel->setText("A code was sent via Telegram to your other\ndevices, if you have any connected.");
//                break;
//            case TLType::AuthSentCodeTypeSms:
//                codeNumberDescriptionLabel->setText("We've sent an activation code to your phone.\nPlease enter it below.");
//                break;
//            case TLType::AuthSentCodeTypeCall:
//                break;
//            case TLType::AuthSentCodeTypeFlashCall:
//                break;
//            case TLType::AuthSentCodeTypeMissedCall:
//                break;
//            case TLType::AuthSentCodeTypeEmailCode:
//                break;
//            case TLType::AuthSentCodeTypeSetUpEmailRequired:
//                //TODO: show error or implement it lol
//                break;
//            case TLType::AuthSentCodeTypeFragmentSms:
//                break;
//            case TLType::AuthSentCodeTypeFirebaseSms:
//                break;
            }

            authScreen.currentIndex = 2;
        }

        onAuthAuthorizationResponse: {
            setAuthProgress(false);

            if (data["_"] == 0x44747e9a) {
                //TODO sign up / registration support
                snackBar.text = "Sign up isn't supported now. Please, use official app for signing up.";
            }
        }

        onAuthorized: {
            setAuthProgress(false);
            //TODO hide reconnecting

            root.state = "MAIN";
        }

        onRpcError: {
            setAuthProgress(false);

            //TODO think how to improve it
            if (errorMessage == "PHONE_NUMBER_INVALID") {
                snackBar.text = "Invalid phone number. Please try again.";
            }
            else if (errorMessage == "PHONE_NUMBER_FLOOD") {
                snackBar.text = "Phone is used too many times recently.";
            }
            else if (errorMessage == "PHONE_CODE_INVALID") {
                snackBar.text = "You have entered an invalid code.";
            }
            else if (errorMessage == "PASSWORD_HASH_INVALID") {
                authScreen.passwordPage.progress = -1;
                snackBar.text = "That password is not right.";
            }
            else if (errorMessage == "SRP_PASSWORD_CHANGED") {
                //The password changed under us, so the one just typed is now
                //known to be wrong. Refresh the hint rather than retrying.
                authScreen.passwordPage.progress = -1;
                snackBar.text = "The cloud password has changed. Please try again.";
                telegramClient.accountGetPassword();
            }
            else if (errorMessage.indexOf("FLOOD_WAIT_") == 0) {
                //Telegram rate-limits per method, so this arrives for ordinary
                //background work -- avatar and media fetches -- as readily as
                //for a login attempt. Only the login flow can honestly call
                //them attempts; saying so after a successful login reports a
                //failure that did not happen. The generic branch below would
                //print FLOOD_WAIT_31, which is not a sentence either.
                var floodSeconds = errorMessage.substring(11);

                if (root.state == "AUTH") {
                    authScreen.passwordPage.progress = -1;
                    snackBar.text = "Too many attempts. Please wait "
                            + floodSeconds + " seconds.";
                }
                else {
                    //The transport replays a flood-waited request on its next
                    //ping tick, so this is a delay rather than a loss.
                    snackBar.text = "Telegram is rate-limiting requests. Retrying in "
                            + floodSeconds + "s.";
                }
            }
            else {
                snackBar.text = "RPC error occured: " + errorMessage + " (" + errorCode + ")"
            }
        }

        onSocketError: {
            // Only worth reporting while the user is waiting on a login. Once
            // there is a session, onReconnecting says the same thing with the
            // delay attached, and reporting both means two messages per drop.
            if (state == "AUTH") {
                setAuthProgress(false);
                snackBar.text = "Socket error occured: " + errorMessage + " (" + errorCode + ")"
            }
        }

        onTfaRequired: {
            setAuthProgress(false);

            authScreen.passwordPage.reset();
            authScreen.currentIndex = 3;

            //Fetched now only to show the hint. The proof fetches its own
            //parameters when the user submits, because srp_id expires and a
            //stale one is refused as though the password were wrong.
            telegramClient.accountGetPassword();
        }

        onAccountPasswordResponse: {
            authScreen.passwordPage.hint = data["hint"] ? data["hint"] : "";
        }

        onPasswordCheckProgress: {
            authScreen.passwordPage.progress = percent;
        }

        onPasswordCheckFailed: {
            setAuthProgress(false);
            authScreen.passwordPage.progress = -1;
            snackBar.text = reason;
        }

        onHelpCountriesListResponse: {
            //TODO country selector
        }

        //Debug only
//        onFileDownloadCanceled: {
//            console.log("[INFO] File " + fileId + " download canceled");
//        }

//        onFileDownloaded: {
//            console.log("[INFO] File " + fileId + " have been downloaded");
//        }

//        onFileDownloading: {
//            console.log("[INFO] File " + fileId + " download progress: " + processedLength + " / " + totalLength + " " + progressPercentage + " %");
//        }

//        onFileUploadCanceled: {
//            console.log("[INFO] File " + fileId + " upload canceled");
//        }

//        onFileUploaded: {
//            console.log("[INFO] File " + fileId + " have been uploaded");
//        }

//        onFileUploading: {
//            console.log("[INFO] File " + fileId + " upload progress: " + processedLength + " / " + totalLength + " " + progressPercentage + " %");
//        }
    }

    // The transport backs off after a failed connect, so without this it would
    // wait out a delay chosen while the phone had no signal. PlatformUtils is
    // where bearer state is known; the transport only knows its socket failed.
    Connections {
        target: platformUtils
        onNetworkOnline: {
            if (telegramClient.hasSession()) {
                telegramClient.retryNow();
            }
        }
    }

    AvatarDownloader {
        id: globalAvatarDownloader
        client: telegramClient
    }

    AuthScreen {
        id: authScreen
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: parent.width
    }

    MainScreen {
        id: mainScreen
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: parent.width
    }

    SnackBar {
        id: snackBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }

    Keys.onPressed: {
        if (event.key == Qt.Key_Context1 || event.key == Qt.Key_Escape) {
            topBar.menuButtonClicked();
        }
    }
}
