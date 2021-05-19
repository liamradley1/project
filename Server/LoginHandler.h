#pragma once
#include "Account.h"
class LoginHandler {
private:
	Account* loggedAccount;

public:

	LoginHandler();
	
	bool isLoggedIn();

	void logout();

	Account* getLogged();

	void setLogged(Account* account);
};