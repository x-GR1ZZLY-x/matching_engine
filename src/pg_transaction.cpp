#include"pg_transaction.hpp"

namespace matching_engine{

PgTransaction::PgTransaction(PgConnection& connection)
    : connection_(connection), committed_(false){
    // Счётчик увеличивается только после успешного BEGIN — если BEGIN
    // бросит, конструктор PgTransaction не достроится и его деструктор не
    // выполнится, а значит decrement был бы недостижим.
    connection_.execute("BEGIN");
    connection_.addActiveTransaction();
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
    connection_.removeActiveTransaction();
}

}
