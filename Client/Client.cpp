#include <cpprest/http_client.h>
#include <cpprest/filestream.h>
#include <cpprest/uri.h>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <codecvt>
#include <locale>
#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <croncpp/croncpp.h>
#include <excpt.h>
#include <cpprest/http_listener.h>

using namespace std;
using namespace web;
using namespace web::http;
using namespace web::http::client;
using namespace web::http::experimental::listener;
using namespace cron;

using namespace concurrency::streams;

static wstring loggedID = L"";

#define KEY_LENGTH 2048 // Key length
#define PUB_KEY_FILE "clientRSApub.pem" // public key path
#define PRI_KEY_FILE "clientRSApri.pem" // private key path
#define AES_BITS 256

/* A 256 bit key */
unsigned char* aesKey = new unsigned char[AES_BITS];

/* A 128 bit IV */
unsigned char* iv = new unsigned char[AES_BITS / 2];

// Generates sessional RSA key
void GenerateRSAKey(std::string& out_pub_key, std::string& out_pri_key)
{
    size_t pri_len = 0; // Private key length
    size_t pub_len = 0; // public key length
    char* pri_key = nullptr; // private key
    char* pub_key = nullptr; // public key

    RSA* keypair = RSA_generate_key(KEY_LENGTH, RSA_3, NULL, NULL);

    BIO* pri = BIO_new(BIO_s_mem());
    BIO* pub = BIO_new(BIO_s_mem());

    PEM_write_bio_RSAPrivateKey(pri, keypair, NULL, NULL, 0, NULL, NULL);
    PEM_write_bio_RSA_PUBKEY(pub, keypair);
    pri_len = BIO_pending(pri);
    pub_len = BIO_pending(pub);

    pri_key = (char*)malloc(pri_len + 1);
    pub_key = (char*)malloc(pub_len + 1);

    BIO_read(pri, pri_key, pri_len);
    BIO_read(pub, pub_key, pub_len);

    pri_key[pri_len] = '\0';
    pub_key[pub_len] = '\0';

    out_pub_key = pub_key;
    out_pri_key = pri_key;

    std::ofstream pub_file(PUB_KEY_FILE, std::ios::out);
    if (!pub_file.is_open())
    {
        perror("pub key file open fail:");
        return;
    }
    pub_file << pub_key;
    pub_file.close();

    std::ofstream pri_file(PRI_KEY_FILE, std::ios::out);
    if (!pri_file.is_open())
    {
        perror("pri key file open fail:");
        return;
    }
    pri_file << pri_key;
    pri_file.close();

    RSA_free(keypair);
    BIO_free_all(pub);
    BIO_free_all(pri);

    free(pri_key);
    free(pub_key);
}

// Encrypts data with the RSA private key
std::string RsaPriEncrypt(const std::string& clear_text, std::string& pri_key)
{
    std::string encrypt_text;
    BIO* keybio = BIO_new_mem_buf((unsigned char*)pri_key.c_str(), -1);
    RSA* rsa = RSA_new();
    rsa = PEM_read_bio_RSAPrivateKey(keybio, &rsa, NULL, NULL);
    if (!rsa)
    {
        BIO_free_all(keybio);
        throw new exception("RSA Private Key Encryption Error.");
    }

    int len = RSA_size(rsa);

    char* text = new char[len + 1];
    memset(text, 0, len + 1);

    int ret = RSA_private_encrypt(clear_text.length(), (const unsigned char*)clear_text.c_str(), (unsigned char*)text, rsa, RSA_PKCS1_PADDING);
    if (ret >= 0) {
        encrypt_text = std::string(text, ret);
    }

    delete[] text;
    BIO_free_all(keybio);
    RSA_free(rsa);

    return encrypt_text;
}

// Decrypts data with the RSA public key
std::string RsaPubDecrypt(const std::string& cipher_text, const std::string& pub_key)
{
    std::string decrypt_text;
    BIO* keybio = BIO_new_mem_buf((unsigned char*)pub_key.c_str(), -1);
    RSA* rsa = RSA_new();

    rsa = PEM_read_bio_RSA_PUBKEY(keybio, &rsa, NULL, NULL);
    if (!rsa)
    {
        unsigned long err = ERR_get_error(); 
        char err_msg[1024] = { 0 };
        ERR_error_string(err, err_msg);
        cout << "err msg: err:" << err << ", msg:" << err_msg << endl;
        BIO_free_all(keybio);
        throw new exception("RSA Public Decryption Error");
    }

    int len = RSA_size(rsa);
    char* text = new char[len + 1];
    memset(text, 0, len + 1);
    int ret = RSA_public_decrypt(cipher_text.length(), (const unsigned char*)cipher_text.c_str(), (unsigned char*)text, rsa, RSA_PKCS1_PADDING);
    if (ret >= 0) {
        decrypt_text.append(std::string(text, ret));
    }

    delete[] text;
    BIO_free_all(keybio);
    RSA_free(rsa);

    return decrypt_text;
}

// Encrypts data with the RSA public key
std::string RsaPubEncrypt(const std::string& clear_text, const std::string& pub_key)
{
    std::string encrypt_text;
    BIO* keybio = BIO_new_mem_buf((unsigned char*)pub_key.c_str(), -1);
    RSA* rsa = RSA_new();
    rsa = PEM_read_bio_RSA_PUBKEY(keybio, &rsa, NULL, NULL);

    int key_len = RSA_size(rsa);
    int block_len = key_len - 11; // Done because we use PKCS1 padding

    char* sub_text = new char[key_len + 1];
    memset(sub_text, 0, key_len + 1);
    int ret = 0;
    int pos = 0;
    std::string sub_str;
    while (pos < clear_text.length()) {
        sub_str = clear_text.substr(pos, block_len);
        memset(sub_text, 0, key_len + 1);
        ret = RSA_public_encrypt(sub_str.length(), (const unsigned char*)sub_str.c_str(), (unsigned char*)sub_text, rsa, RSA_PKCS1_PADDING);
        if (ret >= 0) {
            encrypt_text.append(std::string(sub_text, ret));
        }
        pos += block_len;
    }

    BIO_free_all(keybio);
    RSA_free(rsa);
    delete[] sub_text;

    return encrypt_text;
}

// Decrypts the data with the RSA private key
std::string RsaPriDecrypt(const std::string& cipher_text, const std::string& pri_key)
{
    std::string decrypt_text;
    RSA* rsa = RSA_new();
    BIO* keybio;
    keybio = BIO_new_mem_buf((unsigned char*)pri_key.c_str(), -1);

    rsa = PEM_read_bio_RSAPrivateKey(keybio, &rsa, NULL, NULL);
    if (rsa == nullptr) {
        unsigned long err = ERR_get_error();
        char err_msg[1024] = { 0 };
        throw new exception("RSA Private Key Decryption Error");
    }

    int key_len = RSA_size(rsa);
    char* sub_text = new char[key_len + 1];
    memset(sub_text, 0, key_len + 1);
    int ret = 0;
    std::string sub_str;
    int pos = 0;
    while (pos < cipher_text.length()) {
        sub_str = cipher_text.substr(pos, key_len);
        memset(sub_text, 0, key_len + 1);
        ret = RSA_private_decrypt(sub_str.length(), (const unsigned char*)sub_str.c_str(), (unsigned char*)sub_text, rsa, RSA_PKCS1_PADDING);
        if (ret >= 0) {
            decrypt_text.append(std::string(sub_text, ret));
            pos += key_len;
        }
    }
    delete[] sub_text;
    BIO_free_all(keybio);
    RSA_free(rsa);

    return decrypt_text;
}

// Error handling for AES openssl methods
void handleErrors(void)
{
    ERR_print_errors_fp(stderr);
}

// Low-level encryption via AES
int encrypt(unsigned char* plaintext, int plaintext_len, unsigned char* key,
    unsigned char* iv, unsigned char* ciphertext)
{
    EVP_CIPHER_CTX* ctx;

    int len;

    int ciphertext_len;

    if (!(ctx = EVP_CIPHER_CTX_new()))
        handleErrors();


    if (1 != EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv))
        handleErrors();

    if (1 != EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len))
        handleErrors();
    ciphertext_len = len;

    if (1 != EVP_EncryptFinal_ex(ctx, ciphertext + len, &len))
        handleErrors();
    ciphertext_len += len;

    EVP_CIPHER_CTX_free(ctx);

    return ciphertext_len;
}

// Low-level decryption via AES
int decrypt(unsigned char* ciphertext, int ciphertext_len, unsigned char* key,
    unsigned char* iv, unsigned char* plaintext)
{
    EVP_CIPHER_CTX* ctx;

    int len;

    int plaintext_len;

    if (!(ctx = EVP_CIPHER_CTX_new()))
        handleErrors();

    if (1 != EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv))
        handleErrors();

    if (1 != EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len))
        handleErrors();
    plaintext_len = len;

    if (1 != EVP_DecryptFinal_ex(ctx, plaintext + len, &len))
        handleErrors();
    plaintext_len += len;

    EVP_CIPHER_CTX_free(ctx);

    return plaintext_len;
}

// High-level encryption via AES for use with cppRESTSDK
wstring aesEncrypt(string input) {
    unsigned char* plaintext = new unsigned char[input.length() * 16];
    unsigned char* ciphertext = new unsigned char[input.length() * 16];
    plaintext = reinterpret_cast<unsigned char*>(const_cast<char*>(input.c_str()));
    int ciphertext_len;
    ciphertext_len = encrypt(plaintext, input.length(), aesKey, iv, ciphertext);
    wstring toSend = L"";
    toSend += to_wstring(ciphertext_len) + L",";
    for (int i = 0; i < ciphertext_len; ++i) {
        toSend += to_wstring((int)ciphertext[i]) + L",";
    }
    return toSend;
}

// High-level encryption via AES for use with cppRESTSDK
string aesDecrypt(wstring input) {
    int index = input.find_first_of(L",");
    int ciphertext_len = stoi(input.substr(0, index));
    wstring body = input.substr(index + 1, input.length());
    unsigned char* ciphertext = new unsigned char[ciphertext_len];
    unsigned char* plaintext = new unsigned char[ciphertext_len];
    for (int i = 0; i < ciphertext_len; ++i) {
        index = body.find_first_of(L",");
        int toAdd = stoi(body.substr(0, index));
        body = body.substr(index + 1, body.length());
        ciphertext[i] = (unsigned char)toAdd;
    }
    int plaintext_len = decrypt(ciphertext, ciphertext_len, aesKey, iv, plaintext);
    string final = "";
    for (int i = 0; i < plaintext_len; ++i) {
        final += (char)((int)plaintext[i]);
    }
    return final;
}

// Gets the DNS of the server from its file
wstring readServerDNS() {
    try {
        ifstream inFile("serverDNS.txt");
        string location;
        inFile >> location;
        return wstring_convert<codecvt_utf8<wchar_t>>().from_bytes(location);
    }
    catch (exception& e) {
        cout << e.what() << endl;
    }
}

// Read in the serverDNS as a global variable
wstring serverDNS = readServerDNS();

// Key negotiation method for use with central server
web::http::status_code getKeys(string pubKey, string priKey, unsigned char* aesKey, unsigned char* iv) {
    try {
        http_client client(serverDNS + L":8080/requestkey");
        wstring toSend = L"";
        for (int i = 0; i < pubKey.length(); ++i) {
            int converted = (int)pubKey.at(i);
            toSend += to_wstring(converted) + L",";
        }
        auto response = client.request(methods::POST, to_wstring(pubKey.length()), toSend).get();
        if (response.status_code() == status_codes::OK) {
            string ciphertext = response.extract_utf8string().get();
            string extracted_key = RsaPriDecrypt(ciphertext, priKey);
            string finalString = "";
            for (int i = 0; i < AES_BITS; ++i) {
                int index = extracted_key.find_first_of(",");
                int toAdd = stoi(extracted_key.substr(0, index));
                extracted_key = extracted_key.substr(index + 1, extracted_key.length());
                aesKey[i] = (unsigned char)toAdd;
            }
            extracted_key = extracted_key.substr(1, extracted_key.length());
            for (int i = 0; i < AES_BITS / 2; ++i) {
                int index = extracted_key.find_first_of(",");
                int toAdd = stoi(extracted_key.substr(0, index));
                extracted_key = extracted_key.substr(index + 1, extracted_key.length());
                iv[i] = (unsigned char)toAdd;
            }
            return response.status_code();
        }
        else {
            wcout << response.extract_utf16string().get() << endl;
            return response.status_code();
        }
    }
    catch (exception& e) {
        cout << e.what() << endl;
    }
}

// Sends encrypted login details to the central server
web::http::status_code sendLogin(wstring id, wstring pin) {
    http_client client(serverDNS + L":8080/login");
    cout << "Sending..." << endl;
    auto response = client.request(methods::PUT, id, pin).get();
    if (response.status_code() == status_codes::OK) {
        loggedID = id;
        cout << "Logged in!" << endl;
    }
    else {
        wcout << response.extract_utf16string().get() << endl;
    }
    cout << flush;
    return response.status_code();
}

// Sends encrypted transaction request to the central server. Receives information about success of transaction
status_code sendTransfer() {
    string accountId;
    string amount;
    cout << "Which account would you like to send to?" << endl;
    getline(cin, accountId);
    cout << "How much would you like to send?" << endl;
    cout << "Amount: " << (char)156 << flush;
    getline(cin, amount);
    string accFrom = wstring_convert<codecvt_utf8<wchar_t>>().to_bytes(loggedID);
    string toEncrypt = accFrom + "," + accountId;
    wstring ids = aesEncrypt(toEncrypt);
    wstring amountToSend = aesEncrypt(amount);
    http_client client(serverDNS + L":8080/transfer");
    auto response = client.request(methods::POST, ids, amountToSend).get();
    if (response.status_code() == status_codes::OK) {
        system("CLS");
        cout << "Transfer successful!" << endl;
    }
    else if (response.status_code() == status_codes::InternalError) {
        system("CLS");
        cout << "An error occurred on the server. Please try again later." << endl;
    }
    else {
        system("CLS");
        wcout << response.extract_utf16string().get() << endl;
    }
    return response.status_code();
}

// Sends encrypted balance request to the central server. Receives encrypted balance
status_code checkBalance() {
    try {
        http_client client(serverDNS + L":8080/transfer");
        string toEnc = wstring_convert<codecvt_utf8<wchar_t>>().to_bytes(loggedID);
        wstring toSend = aesEncrypt(toEnc);
        auto response = client.request(methods::GET, toSend).get();
        if (response.status_code() == status_codes::OK) {
            wstring body = response.extract_utf16string().get();
            double balance = stod(aesDecrypt(body));
            system("CLS");
            cout << "Balance: " << (char)156 << setprecision(2) << fixed << balance << endl << endl;
        }
        else if (response.status_code() == status_codes::InternalError) {
            system("CLS");
            cout << "An error has occurred on the server. Please try again later." << endl;
        }
        else {
            system("CLS");
            wcout << response.extract_utf16string().get() << endl;
        }
        return response.status_code();
    }
    catch (exception& e) {
        system("CLS");
        cout << e.what() << endl;
    }
}

// Sends encrypted transaction history request to the central server. Receives encrypted transaction history
status_code checkHistory() {
    try {
        http_client client(serverDNS + L":8080/history");
        string toEncrypt = wstring_convert<codecvt_utf8<wchar_t>>().to_bytes(loggedID);
        wstring toSend = aesEncrypt(toEncrypt);
        auto response = client.request(methods::GET, toSend).get();
        if (response.status_code() == status_codes::OK) {
            wstring body = response.extract_utf16string().get();
            string history = aesDecrypt(body);
            system("CLS");
            cout << history << endl;
        }
        else {
            system("CLS");
            wcout << response.extract_utf16string().get() << endl;
        }
        return response.status_code();
    }
    catch (exception& e) {
        system("CLS");
        cout << e.what() << endl;
        cout << "Please try again." << endl;
    }
    return status_codes::InternalError;
}

// Sends encrypted direct debit addition request to the central server. Receives information about success of transaction
status_code addDebit() {
    http_client client(serverDNS + L":8080/debits");
    try {
        std::string idTo;
        int intIdTo;
        wstring id;
        while (true) {
            std::cout << "Enter the id of the account you want to transfer to." << std::endl;
            std::getline(std::cin, idTo);
            intIdTo = stoi(idTo);
            id = to_wstring(intIdTo);
            break;
        }
        std::string choice;
        std::string regString;
        while (true) {
            std::cout << "Please enter the regularity of the payment:" << std::endl;
            std::cout << "1: Once a minute" << std::endl;
            std::cout << "2: Once an hour" << std::endl;
            std::cout << "3: Once a day" << std::endl;
            std::cout << "4: Once a week" << std::endl;
            std::cout << "5: On the first day of every month" << std::endl;
            std::cout << "6: Once a year" << std::endl;
            std::getline(std::cin, choice);
            if (choice == "1") {
                regString = "0 * * * * ?";
                break;
            }
            else if (choice == "2") {
                regString = "0 0 * * * ?";
                break;
            }
            else if (choice == "3") {
                regString = "0 0 0 * * ?";
                break;
            }
            else if (choice == "4") {
                regString = "0 0 0 * * 1";
                break;
            }
            else if (choice == "5") {
                regString = "0 0 0 1 * *";
                break;
            }
            else if (choice == "6") {
                regString = "0 0 0 1 1 ?";
                break;
            }
            else {
                system("CLS");
                std::cout << "Invalid choice. Please try again." << std::endl;
            }
        }
        wstring regularity = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(regString);
        double amount = 0;
        std::wstring amountString;
        while (amount <= 0) {
            std::cout << "Enter the amount you wish to transfer: " << std::endl;
            std::cout << "Amount: " << (char)156 << std::flush;
            getline(std::wcin, amountString);
            amount = stod(amountString);
        }
        wstring collection = id + L"," + regularity + L"," + amountString;
        wstring toSend = aesEncrypt(wstring_convert<codecvt_utf8<wchar_t>>().to_bytes(collection));
        wstring encId = aesEncrypt(wstring_convert<codecvt_utf8<wchar_t>>().to_bytes(loggedID));
        auto response = client.request(methods::POST, encId, toSend);
        system("CLS");
        wcout << response.get().extract_utf16string().get() << endl;
    }
    catch (std::exception& e) {
        system("CLS");
        std::cout << "Something went wrong!" << std::endl;
        std::cout << e.what() << std::endl;
    }
    return status_codes::OK;
}

// Sends encrypted direct debit viewing request to the central server. Receives encrypted debit information
void viewDebits() {
    try {
        http_client client(serverDNS + L":8080/debits");
        string toEncrypt = wstring_convert<codecvt_utf8<wchar_t>>().to_bytes(loggedID);
        wstring toSend = aesEncrypt(toEncrypt);
        auto response = client.request(methods::GET, toSend);
        if (response.get().status_code() == status_codes::OK) {
            wstring body = response.get().extract_utf16string().get();
            string details = aesDecrypt(body);
            system("CLS");
            cout << details << endl;
        }
        else {
            system("CLS");
            wcout << response.get().extract_utf16string().get() << endl;
        }
    }
    catch (exception& e) {
        system("CLS");
        cout << e.what() << endl;
        cout << "Request timed out. Please try again." << endl;
    }
}

// Sends encrypted direct debit delete request to the central server. Receives information about success of transaction
void removeDebit() {
    cout << "Which debit ID would you like to remove?" << endl;
    string input = "";
    getline(cin, input);
    http_client client(serverDNS + L":8080/debits");
    wstring toSend = aesEncrypt(input);
    wstring idToSend = aesEncrypt(wstring_convert<codecvt_utf8<wchar_t>>().to_bytes(loggedID));
    auto response = client.request(methods::DEL, idToSend, toSend).get();
    system("CLS");
    wcout << response.extract_utf16string().get() << endl;
}

// Debit menu for UI
status_code debitMenu() {
    while (true) {
        cout << "Select the option you would like to perform." << endl;
        cout << "1: Add a direct debit \n2: View direct debits on this account \n3: Remove a direct debit\n4: Go back to the previous menu" << endl;
        string choice = "0";
        getline(cin, choice);
        if (choice.compare("1") == 0) {
            addDebit();
        }
        else if (choice.compare("2") == 0) {
            viewDebits();
        }
        else if (choice.compare("3") == 0) {
            removeDebit();
        }
        else if (choice.compare("4") == 0) {
            return status_codes::OK;
        }
        else {
            system("CLS");
            cout << "Invalid choice. Please try again" << endl;
        }
    }
}

// Sends heartbeat to central server to inform that it is still logged in. Exits application if unsuccessful
void heartbeat() {
    try {
        while (loggedID.compare(L"") != 0) {
            http_client client(serverDNS + L":8080/heartbeat");
            auto response = client.request(methods::GET).get();
            if (response.status_code() != status_codes::OK) {
                wcout << response.extract_utf16string().get() << endl;
                system("CLS");
                cout << "Heartbeat could not be sent" << endl;
                exit(1);
            }
            _sleep(10000);
        }
    }
    catch (exception& e) {
        system("CLS");
        cout << "Heartbeat could not be sent" << endl;
        cout << e.what() << endl;
        exit(1);
    }
}

// Sends encrypted logout request to central server.
status_code sendLogout() {
    http_client client(serverDNS + L":8080/login");
    string toEncrypt = wstring_convert<codecvt_utf8<wchar_t>>().to_bytes(loggedID);
    wstring toSend = aesEncrypt(toEncrypt);
    auto response = client.request(methods::DEL, toSend).get();
    if (response.status_code() == status_codes::OK) {
        loggedID = L"";
    }
    else {
        system("CLS");
        wcout << response.extract_utf16string().get() << endl;
    }
    return response.status_code();
}

int main()
{
    try {
        string pub_key;
        string priv_key;
        GenerateRSAKey(pub_key, priv_key);
        status_code code;
        code = getKeys(pub_key, priv_key, aesKey, iv);
        if (code == status_codes::OK) {
            do {
                code = status_codes::BadRequest;
                cout << "Enter your account id." << endl;
                string id;
                string pin;
                cout << "id: " << flush;
                getline(cin, id);
                cout << "pin: " << flush;
                getline(cin, pin);
                std::hash<int> hash;
                size_t hashed;
                int pinNum = 0;
                try {
                    pinNum = stoi(pin);
                }
                catch (exception& e) {
                    system("CLS");
                    cout << "Invalid login details. Please try again." << endl;
                }
                if (pinNum != 0) {
                    hashed = hash(std::stoi(pin));
                    pin = to_string(hashed);
                    wstring idToSend = aesEncrypt(id);
                    wstring pinToSend = aesEncrypt(pin);
                    code = sendLogin(idToSend, pinToSend);
                    if (code == status_codes::OK) {
                        loggedID = wstring_convert < codecvt_utf8<wchar_t>>().from_bytes(id);
                    }
                }
            } while (code != status_codes::OK);
            int in = 0;
            system("CLS");
            // Start the heartbeat thread to keep the account logged in.
            std::thread heartbeatThread(heartbeat);
            do {
                cout << "What do you want to do?" << endl;
                std::cout << "1: Make a transfer.\n2: Check balance.\n3: Check transaction history.\n4: Add or remove direct debits.\n5: Log out." << std::endl;
                std::cout << "Choice: " << std::flush;
                std::string input;
                std::getline(std::cin, input);
                try {
                    in = stoi(input);
                }
                catch (exception& e) {
                    system("CLS");
                    cout << "Please enter a number when trying to perform actions." << endl;
                }
                switch (in) {
                case 1:
                    sendTransfer();
                    break;
                case 2:
                    checkBalance();
                    break;
                case 3:
                    checkHistory();
                    break;
                case 4:
                    debitMenu();
                    break;
                case 5:
                    system("CLS");
                    std::cout << "Logging out..." << std::endl;
                    sendLogout();
                    heartbeatThread.join();
                    break;
                default:
                    system("CLS");
                    std::cout << "Invalid choice. Please try again." << std::endl;
                    break;
                }
            } while (in != 5);
        }
        else {
            system("CLS");
            cout << "Unable to connect to the server." << endl;
        }
    }
    catch (exception& e) {
        system("CLS");
        cout << e.what() << endl;
    }
    // Delete key and IV to prevent memory leaks
    system("CLS");
    cout << "Goodbye!" << endl;
    delete[] aesKey;
    delete[] iv;
}