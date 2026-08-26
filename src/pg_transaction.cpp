#include"pg_transaction.hpp"

namespace matching_engine{

PgTransaction::PgTransaction(PgConnection& connection)
    : connection_(connection), committed_(false){
    connection_.execute("BEGIN");
}

void PgTransaction::commit(){
    connection_.execute("COMMIT");
    committed_ = true;
}

PgTransaction::~PgTransaction(){
    if(!committed_){
        try{
            connection_.execute("ROLLBACK");
        } catch(...){
        }
    }
}

}
