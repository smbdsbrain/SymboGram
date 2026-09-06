#include "tgclient.h"

#include "apisecrets.h"
#include "tlschema.h"

TgLongVariant TgClient::authSendCode(QString phoneNumber)
{
    TGOBJECT(TLType::AuthSendCodeMethod, method);

    TGOBJECT(TLType::CodeSettings, codeSettings);
    method["settings"] = codeSettings;

    method["phone_number"] = phoneNumber;

#if defined(SYMBOGRAM_API_ID)
    method["api_id"] = SYMBOGRAM_API_ID;
#else
    #error "Please, specify an API id."
#endif

#if defined(SYMBOGRAM_API_HASH)
    method["api_hash"] = SYMBOGRAM_API_HASH;
#else
    #error "Please, specify an API hash."
#endif

    return sendObject<&writeTLMethodAuthSendCode>(method);
}

TgLongVariant TgClient::authSignIn(QString phoneNumber, QString phoneCodeHash, QString phoneCode)
{
    TGOBJECT(TLType::AuthSignInMethod, method);

    method["phone_number"] = phoneNumber;
    method["phone_code_hash"] = phoneCodeHash;
    method["phone_code"] = phoneCode;

    return sendObject<&writeTLMethodAuthSignIn>(method);
}

TgLongVariant TgClient::authSignUp(QString phoneNumber, QString phoneCodeHash, QString firstName, QString lastName)
{
    TGOBJECT(TLType::AuthSignUpMethod, method);

    method["phone_number"] = phoneNumber;
    method["phone_code_hash"] = phoneCodeHash;
    method["first_name"] = firstName;
    method["last_name"] = lastName;

    return sendObject<&writeTLMethodAuthSignUp>(method);
}

TgLongVariant TgClient::accountGetPassword()
{
    TGOBJECT(TLType::AccountGetPasswordMethod, method);

    return sendObject<&writeTLMethodAccountGetPassword>(method);
}

TgLongVariant TgClient::authCheckPassword(TgLongVariant srpId, QByteArray a, QByteArray m1)
{
    TGOBJECT(TLType::AuthCheckPasswordMethod, method);

    TGOBJECT(TLType::InputCheckPasswordSRP, password);
    password["srp_id"] = srpId;
    password["A"] = a;
    password["M1"] = m1;

    method["password"] = password;

    return sendObject<&writeTLMethodAuthCheckPassword>(method);
}
