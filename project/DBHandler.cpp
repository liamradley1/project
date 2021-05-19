#include "DBHandler.h"
#include <croncpp/croncpp.h>
#include <string>
#include <fstream>
using namespace mysqlx;

DBHandler::DBHandler(LoginHandler* log, TransactionHandler* tran)
{
	this->log = log;
	this->tran = tran;
	this->schema = nullptr;
	this->session = nullptr;
	this->accounts = nullptr;
	this->transactions = nullptr;
	this->debits = nullptr;
}

bool DBHandler::logAndHandleTransaction(Account* from, Account* to, double amount, seal::PublicKey public_key, seal::SEALContext context, seal::EncryptionParameters params)
{
	if (tran->transaction(from, to, amount, context, params)) {
		session->startTransaction();
		try {
			std::time_t nowTime = std::time(nullptr);
			seal::Encryptor enc(context, public_key);
			seal::Ciphertext cTo, cFrom;
			seal::CKKSEncoder encoder(context);
			seal::Plaintext plain;
			seal::Evaluator eval(context);
			double scale = pow(2, 20);
			encoder.encode(amount, scale, plain);
			enc.encrypt(plain, cTo);
			enc.encrypt(plain, cFrom);
			eval.negate_inplace(cFrom);
			std::string transactionAddressFrom = std::to_string(from->getId()) + "'" + std::to_string(to->getId()) + "'" + std::to_string(nowTime) + ".txt";
			std::string transactionAddressTo = std::to_string(to->getId()) + "'" + std::to_string(from->getId()) + "'" + std::to_string(nowTime) + ".txt";
			std::ofstream streamFrom(transactionAddressFrom, std::ios::binary);
			std::ofstream streamTo(transactionAddressTo, std::ios::binary);
			cTo.save(streamTo);
			cFrom.save(streamFrom);
			transactions->insert("transactionTime", "transactionType", "amount", "transactionOwnerID", "otherAccountID").values(nowTime, "debit", transactionAddressFrom, from->getId(), to->getId()).execute();
			transactions->insert("transactionTime", "transactionType", "amount", "transactionOwnerID", "otherAccountID").values(nowTime, "credit", transactionAddressTo, to->getId(), from->getId()).execute();
			session->commit();
			return true;
		}
		catch (const Error& e) {
			std::cout << "Error caught: " << e.what() <<std::endl;
			session->rollback();
			return false;
		}
	}
	return false;
}

bool DBHandler::connectToDB()
{
	try {
		Session* session = new Session( mysqlx::SessionOption::USER, "root",
			mysqlx::SessionOption::PWD, "admin",
			mysqlx::SessionOption::HOST, "localhost",
			mysqlx::SessionOption::PORT, 33060,
			mysqlx::SessionOption::DB, "bankdb"
		);
		Schema* schema = new Schema(*session, "bankdb");
		this->schema = schema;
		this->session = session;
		this->accounts = new Table(*schema, "accounts");
		this->transactions = new Table(*schema, "transactions");
		this->debits = new Table(*schema, "direct_debits");
		return true;
	}
	catch (std::exception& e) {
		std::cout << "Error:" << std::endl;
		std::cout << e.what() << std::endl;
		return false;
	}
}

bool DBHandler::endConnection() {
	try {
		if (session != nullptr && schema != nullptr) {
			session->close();
			schema = nullptr;
			session = nullptr;
			std::cout << "Connection closed." << std::endl;
			return true;
		}
		std::cout << "Connection already closed." << std::endl;
		return false;
	}
	catch (std::exception &e) {
		std::cout << "Error:" << std::endl;
		std::cout << e.what() << std::endl;
		return false;
	}
}

Schema* DBHandler::getSchema() {
	return schema;
}

Session* DBHandler::getSession() {
	return session;
}

Table* DBHandler::getAccounts() {
	return accounts;
}

std::vector<Account*> DBHandler::getAccounts(seal::SEALContext context) {
	RowResult accountNums = accounts->select("id").orderBy("id").execute();
	std::vector<Account*> accounts;
	if (accountNums.count() == 0) {
		std::cout << "No accounts exist." << std::endl;
	}
	else {
		for (Row row : accountNums) {
			int accountId = (int)row.get(0);
			Account* toAdd = getAccount(accountId, context);
			accounts.push_back(toAdd);
		}
	}
	return accounts;
}

Table* DBHandler::getTransactions() {
	return transactions;
}

TransactionList* DBHandler::getTransactions(int accountId, seal::SEALContext context) {
	RowResult tra = transactions->select("*").where("transactionOwnerID=" + std::to_string(accountId)).orderBy("transactionTime").execute();
	if (tra.count() == 0) {
		std::cout << "No transactions have occurred on this account." << std::endl;
		return nullptr;
	}
	else {
		for (Row row : tra) {
			int otherAccountId = (int)row.get(5);
			Account* otherAccount = getAccount(otherAccountId, context);
			Transaction* temp = new Transaction((std::string)row.get(3), log->getLogged(), otherAccount, (std::string)row.get(2), (std::time_t)row.get(1));
			tran->getTransactions()->addTransaction(temp);
		}
		return tran->getTransactions();
	}
	return nullptr;
}

Account* DBHandler::getAccount(int id, seal::SEALContext context) {
	RowResult acc= accounts->select("*").where("id=" + std::to_string(id)).execute();
	if (acc.count() == 1) {
		Row row = acc.fetchOne();
		std::string getBal = (std::string)row.get(3);
		std::string getKey = (std::string)row.get(4);
		Account* accountSearched = new Account((int)row.get(0), (std::string)row.get(1), (std::string)row.get(2), (double)row.get(5), (size_t)row.get(6), getBal, getKey, context);
		return accountSearched;
	}
	else return nullptr;
}

Account* DBHandler::login(seal::SEALContext context) {
	std::string id;
	int intId;
	std::cout << "Enter the account id of the account you want to log in, or type 'q' to close the app." << std::endl;
	std::getline(std::cin, id);
	if (id == "q") {
		exit(0);
	}
	intId = stoi(id);
	Account* toLogin = getAccount(intId, context);
	if (toLogin != nullptr && intId != 1) { // checks account is valid and is not the admin account.
		if (log->login(toLogin)) {
			return toLogin;
		}
		else {
			return nullptr;
		}
	}
	else {
		std::cout << "Invalid id. Please try again." << std::endl;
		return nullptr;
	}
}

LoginHandler* DBHandler::getLog() {
	return log;
}

bool DBHandler::directDebit(DirectDebit* dD, seal::PublicKey public_key, seal::SEALContext context, seal::EncryptionParameters params)
{	
	try {
		if (logAndHandleTransaction(dD->getFrom(), dD->getTo(), dD->getAmount(context, params), public_key, context, params)) {
			return true;
		}
		else {
			removeDebit(dD);
			_sleep(1000);
		}
	}
	catch (std::exception& e) {
		std::cout << e.what() << std::endl;
	}
}

void DBHandler::refreshLogged(seal::SEALContext context)
{
	Account* account = getAccount(log->getLogged()->getId(), context);
	log->setLogged(account);
}

bool DBHandler::addDebit(DirectDebit* d, std::string regString, seal::SEALContext context, seal::EncryptionParameters params)
{
	try {
		session->startTransaction();
		debits->insert("transactionOwnerID", "otherAccountID", "amount", "regularity", "timeSet").values(d->getFrom()->getId(), d->getTo()->getId(), d->getAmountAddress(), regString, d->getTimeSet()).execute();
		session->commit();
		return tran->getDebitList()->addDebit(d);
	}
	catch (Error& e) {
		std::cout << e.what() << std::endl;
		session->rollback();
		return false;
	}
}

void DBHandler::logout() {
	log->logout();
}

DebitList* DBHandler::queryDebits(seal::SEALContext context) {
	try {
		RowResult deb = debits->select("*").execute();
		DebitList* newList = new DebitList();
		if (deb.count() > 0) {
			for (Row r : deb) {
				time_t newTime;
				if ((time_t)r.get(5) < time(nullptr)) {
					time_t now = time(nullptr);
					time_t next = cron::cron_next(cron::make_cron((std::string)r.get(4)), now);
					newTime = cron::cron_next(cron::make_cron((std::string)r.get(4)), time(nullptr));
				}
				else {
					newTime = (time_t)r.get(5);
				}
				DirectDebit* d = new DirectDebit((int)r.get(0), getAccount((int)r.get(1), context), getAccount((int)r.get(2), context), (std::string)r.get(3), cron::make_cron((std::string)r.get(4)), newTime);
				newList->addDebit(d);
				updateDebits(d);
			}
			return newList;
		}
		else {
			return nullptr;
		}
	}
	catch (std::exception& e) {
		std::cout << e.what() << std::endl;
	}
	return nullptr;
}

void DBHandler::updateDebits(DirectDebit* d) {
	try {
		session->startTransaction();
		debits->update().set("timeSet", d->getTimeSet()).where("debitID = :id").bind("id", d->getId()).execute();
		session->commit();
	}
	catch (std::exception& e) {
		std::cout << e.what() << std::endl;
		session->rollback();
	}
}

void DBHandler::refreshDebits(seal::SEALContext context) {
	DebitList* debs = queryDebits(context);
	if (debs != nullptr) {
		tran->setDebitList(debs);
	}
	else {
		tran->setDebitList(new DebitList());
	}
}

void DBHandler::removeDebit(DirectDebit* d)
{
	try {
		session->startTransaction();
		debits->remove().where("debitID = :debitID").bind("debitID", d->getId()).execute();
		session->commit();
	}
	catch (std::exception& e) {
		std::cout << e.what() << std::endl;
		session->rollback();
	}
}

void DBHandler::addInterestTransaction(Account* account, seal::SEALContext context, seal::EncryptionParameters params, seal::PublicKey publicKey) {
	try {
		session->startTransaction();
		double amount;
		double before = account->getBalance(context, params);
		account->accrueInterest(context, params, publicKey);
		double after = account->getBalance(context, params);
		amount = after - before;
		seal::Encryptor enc(context, publicKey);
		seal::CKKSEncoder encoder(context);
		seal::Plaintext p;
		seal::Ciphertext c;
		encoder.encode(amount, p);
		enc.encrypt(p, c);
		time_t nowTime = time(nullptr);
		std::string outputAddress = std::to_string(0) + "'" + std::to_string(account->getId()) + "'" + std::to_string(nowTime);
		std::ofstream output(outputAddress, std::ios::binary);
		c.save(output);
		transactions->insert("transactionTime", "transactionType", "amount", "transactionOwnerID", "otherAccountID").values(nowTime, "Monthly interest", outputAddress, account->getId(), 1).execute();

		session->commit();
	}
	catch (std::exception& e) {
		std::cout << e.what() << std::endl;
		session->rollback();
	}
}
