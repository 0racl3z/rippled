#include "AccountItems.h"

class Offer : public AccountItem
{
	RippleAddress mAccount;
	STAmount mTakerGets;
	STAmount mTakerPays;
	int mSeq;


	Offer(SerializedLedgerEntry::pointer ledgerEntry);	// For accounts in a ledger
public:
	Offer(){}
	virtual ~Offer(){}
	AccountItem::pointer makeItem(const uint160&, SerializedLedgerEntry::ref ledgerEntry);
	LedgerEntryType getType(){ return(ltOFFER); }

	STAmount getTakerPays(){ return(mTakerPays); }
	STAmount getTakerGets(){ return(mTakerGets); }
	RippleAddress getAccount(){ return(mAccount); }
	int getSeq(){ return(mSeq); }

};

// vim:ts=4
