////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2016, goatpig.                                              //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //                                      
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include <thread>
#include <chrono>
#include <ctime>
#include <string.h>
#include "BitcoinP2p.h"

bool PEER_USES_WITNESS;

template <typename T> uint32_t put_integer_be(uint8_t* ptr, const T& integer)
{
   uint32_t size = sizeof(T);
   auto len = size - 1;
   auto intptr = (uint8_t*)&integer;

   for (uint32_t i = 0; i < size; i++)
   {
      ptr[i] = intptr[len - i];
   }

   return size;
};

////////////////////////////////////////////////////////////////////////////////
int get_varint_len(const int64_t& value)
{
   if (value < 0xFD)
      return 1;
   else if (value <= 0xFFFF)
      return 3;
   else if (value <= 0xFFFFFFFF)
      return 5;

   return 9;
}

////////////////////////////////////////////////////////////////////////////////
int make_varint(const uint64_t& value, vector<uint8_t>& varint)
{
   if (value < 0xFD)
   {
      varint.push_back((uint8_t)value);
      return 1;
   }
   else if (value <= 0xFFFF)
   {
      varint.resize(3);
      auto ptr = (uint16_t*)&varint[1];
      *ptr = (uint16_t)value;
      varint[0] = 0xFD;
      return 3;
   }
   else if (value <= 0xFFFFFFFF)
   {
      varint.resize(5);
      auto ptr = (uint32_t*)&varint[1];
      *ptr = (uint32_t)value;
      varint[0] = 0xFE;
      return 5;
   }

   varint.resize(9);
   auto ptr = (uint64_t*)&varint[1];
   *ptr = (uint64_t)value;
   varint[0] = 0xFF;
   return 9;
}

////////////////////////////////////////////////////////////////////////////////
int get_varint(uint64_t& val, uint8_t* ptr, uint32_t size)
{
   if (size == 0)
      throw runtime_error("invalid varint size");

   if (ptr[0] < 0xFD)
   {
      val = *(uint8_t*)(ptr);
      return 1;
   }
   else if (ptr[0] == 0xFD)
   {
      if (size < 3)
         throw runtime_error("invalid varint size");

      val = *(uint16_t*)(ptr + 1);
      return 3;
   }
   else if (ptr[0] == 0xFE)
   {
      if (size < 5)
         throw runtime_error("invalid varint size");

      val = *(uint32_t*)(ptr + 1);
      return 5;
   }

   if (size < 9)
      throw runtime_error("invalid varint size");

   val = *(uint64_t*)(ptr + 1);
   return 9;
}

////////////////////////////////////////////////////////////////////////////////
const map<string, PayloadType> BitcoinP2P::strToPayload_ = {
   make_pair("version", Payload_version),
   make_pair("verack", Payload_verack),
   make_pair("inv", Payload_inv),
   make_pair("ping", Payload_ping),
   make_pair("pong", Payload_pong),
   make_pair("getdata", Payload_getdata),
   make_pair("tx", Payload_tx)
};

////////////////////////////////////////////////////////////////////////////////
////
//// Payload classes
////
////////////////////////////////////////////////////////////////////////////////
vector<uint8_t> Payload::serialize(uint32_t magic_word) const
{
   //serialize payload
   auto payload_size = serialize_inner(nullptr);
   
   vector<uint8_t> msg;
   msg.resize(MESSAGE_HEADER_LEN + payload_size);
   if (payload_size > 0)
      serialize_inner(&msg[MESSAGE_HEADER_LEN]);

   //magic word
   uint8_t* ptr = &msg[0];
   uint32_t* magicword = (uint32_t*)(ptr + MAGIC_WORD_OFFSET);
   *magicword = magic_word;

   //message type
   auto&& type = typeStr();
   char* msgtype = (char*)(ptr + MESSAGE_TYPE_OFFSET);
   memset(msgtype, 0, MESSAGE_TYPE_LEN);
   memcpy(msgtype, type.c_str(), type.size());

   //length
   uint32_t msglen = payload_size;
   uint32_t* msglenptr = (uint32_t*)(ptr + PAYLOAD_LENGTH_OFFSET);
   *msglenptr = msglen;

   //checksum
   uint8_t* payloadptr = nullptr;
   if (payload_size > 0)
      payloadptr = &msg[MESSAGE_HEADER_LEN];
   BinaryDataRef bdr(payloadptr, payload_size);
   auto&& hash = BtcUtils::getHash256(bdr);
   uint32_t* checksum = (uint32_t*)hash.getPtr();
   uint32_t* checksumptr = (uint32_t*)(ptr + CHECKSUM_OFFSET);
   *checksumptr = *checksum;

   return msg;
}

////////////////////////////////////////////////////////////////////////////////
vector<unique_ptr<Payload>> Payload::deserialize(
   vector<uint8_t>& data, uint32_t magic_word)
{
   //TODO: add trailing uncomplete payload data replay in next deser call
   //(mostly useful for grabbing data during blocks over p2p syncing)

   if (data.size() < MESSAGE_HEADER_LEN)
      throw BitcoinMessageDeserError("invalid header size");
   
   size_t offset = 0, totalsize = data.size();
   vector<unique_ptr<Payload>> retvec;

   while (offset < totalsize)
   {
      uint8_t* ptr = &data[offset];

      //check magic word
      uint32_t* magicword = (uint32_t*)(ptr + MAGIC_WORD_OFFSET);
      if (*magicword != magic_word)
      {
         //invalid magic word, search remainder of the packet for another one
         auto sizeRemaining = totalsize - offset;
         auto mwFirstByte = *(uint8_t*)(&magic_word);
         unsigned i;
         for (i = 4; i < sizeRemaining; i++)
         {
            if (ptr[i] == mwFirstByte)
            {
               auto mwPtr = (uint32_t*)(ptr + i);
               if (*mwPtr == magic_word)
                  break;
            }
         }

         offset += i;
         continue;
      }

      //get message type
      char* messagetype = (char*)(ptr + MESSAGE_TYPE_OFFSET);

      //messagetype should be null terminated and no longer than 12 bytes
      int i;
      for (i = 0; i < MESSAGE_TYPE_LEN; i++)
      {
         if (messagetype[i] == 0)
            break;
      }

      if (i == MESSAGE_TYPE_LEN)
      {
         offset += 4; //skip the current mw before reentering the loop
         continue;
      }

      //get and verify length
      uint32_t* length = (uint32_t*)(ptr + PAYLOAD_LENGTH_OFFSET);
      if (*length + MESSAGE_HEADER_LEN > totalsize - offset)
         throw BitcoinMessageDeserError("payload length mismatch");

      //get checksum
      uint32_t* checksum = (uint32_t*)(ptr + CHECKSUM_OFFSET);

      //grab payload
      BinaryDataRef payloadRef(ptr + MESSAGE_HEADER_LEN, *length);

      //verify checksum
      auto&& payloadHash = BtcUtils::getHash256(payloadRef);
      uint32_t* hashChecksum = (uint32_t*)payloadHash.getPtr();

      if (*hashChecksum != *checksum)
      {
         offset += 4;
         continue;
      }

      //instantiate relevant Payload child class and return it
      try
      {
         auto payloadIter = BitcoinP2P::strToPayload_.find(messagetype);
         if (payloadIter != BitcoinP2P::strToPayload_.end())
         {
            uint8_t* payloadptr = nullptr;
            if (*length > 0)
               payloadptr = &data[offset] + MESSAGE_HEADER_LEN;

            switch (payloadIter->second)
            {
            case Payload_version:
               retvec.push_back(move(make_unique<Payload_Version>(
                  payloadptr, *length)));
               break;

            case Payload_verack:
               retvec.push_back(move(make_unique<Payload_Verack>()));
               break;

            case Payload_ping:
               retvec.push_back(move(make_unique<Payload_Ping>(
                  payloadptr, *length)));
               break;

            case Payload_pong:
               retvec.push_back(move(make_unique<Payload_Pong>(
                  payloadptr, *length)));
               break;

            case Payload_inv:
               retvec.push_back(move(make_unique<Payload_Inv>(
                  payloadptr, *length)));
               break;

            case Payload_tx:
               retvec.push_back(move(make_unique<Payload_Tx>(
                  payloadptr, *length)));
               break;

            case Payload_getdata:
               retvec.push_back(move(make_unique<Payload_GetData>(
                  payloadptr, *length)));
               break;
            }
         }
      }
      catch (BitcoinMessageDeserError&)
      {
         //ignore deser error on full length payload
      }
         
      offset += MESSAGE_HEADER_LEN + *length;
   }

   return retvec;
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinNetAddr::deserialize(BinaryRefReader brr)
{
   if (brr.getSize() != NETADDR_NOTIME)
      throw BitcoinMessageDeserError("invalid netaddr size");

   services_ = brr.get_uint64_t(); 
   auto ipv6bdr = brr.get_BinaryDataRef(16);
   memcpy(&ipV6_, ipv6bdr.getPtr(), 16);

   port_ = brr.get_uint16_t();
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinNetAddr::serialize(uint8_t* ptr) const
{
   put_integer_be(ptr, services_);
   memcpy(ptr + 8, ipV6_, 16);
   put_integer_be(ptr + 24, port_);
}

////////////////////////////////////////////////////////////////////////////////
void Payload_Version::deserialize(uint8_t* data, size_t len)
{
   uint8_t* dataptr = data;

   vheader_.version_ = *(uint32_t*)dataptr;
   dataptr += 4;
   vheader_.services_ = *(uint64_t*)dataptr;
   dataptr += 8;
   vheader_.timestamp_ = *(int64_t*)dataptr;
   dataptr += 8;

   vheader_.addr_recv_.services_ = *(uint64_t*)dataptr;
   dataptr += 8;
   memcpy(vheader_.addr_recv_.ipV6_, dataptr, 16);
   dataptr += 16;
   vheader_.addr_recv_.port_ = *(uint16_t*)dataptr;
   dataptr += 2;

   vheader_.addr_from_.services_ = *(uint64_t*)dataptr;
   dataptr += 8;
   memcpy(vheader_.addr_from_.ipV6_, dataptr, 16);
   dataptr += 16;
   vheader_.addr_from_.port_ = *(uint16_t*)dataptr;
   dataptr += 2;

   vheader_.nonce_ = *(uint64_t*)dataptr;
   dataptr += 8;

   size_t remaining = len - MESSAGE_HEADER_LEN - USERAGENT_OFFSET;
   uint64_t uaLen;
   auto varintlen = get_varint(uaLen, dataptr, remaining);
   dataptr += varintlen;
   char* userAgentPtr = (char*)(dataptr);
   userAgent_ = string(userAgentPtr, uaLen);
   dataptr += uaLen;

   startHeight_ = *(uint32_t*)dataptr;
}

////////////////////////////////////////////////////////////////////////////////
size_t Payload_Version::serialize_inner(uint8_t* dataptr) const
{
   if (dataptr == nullptr)
   {
      return get_varint_len(userAgent_.size()) +
         userAgent_.size() +
         VERSION_MINLENGTH;
   }

   vector<uint8_t> varint;
   auto varintlen = make_varint(userAgent_.size(), varint);
   size_t serlen = varintlen + userAgent_.length() + VERSION_MINLENGTH;

   uint8_t* vhptr = dataptr;
   
   put_integer_be(vhptr, vheader_.version_);
   put_integer_be(vhptr +4, vheader_.services_);
   put_integer_be(vhptr +12, vheader_.timestamp_);
   vhptr += 20;

   vheader_.addr_recv_.serialize(vhptr);
   vhptr += 26;

   vheader_.addr_from_.serialize(vhptr);
   vhptr += 26;

   memcpy(vhptr, &vheader_.nonce_, 8);

   uint8_t* ptr = dataptr + USERAGENT_OFFSET;
   memcpy(ptr, &varint[0], varintlen);
   ptr += varintlen;
   memcpy(ptr, userAgent_.c_str(), userAgent_.size());
   memcpy(ptr + userAgent_.size() + 1, &startHeight_, 4);

   return serlen;
}

////////////////////////////////////////////////////////////////////////////////
void Payload_Version::setVersionHeaderIPv4(uint32_t version, uint64_t services,
   int64_t timestamp,
   const sockaddr& recvaddr,
   const sockaddr& fromaddr)
{
   vheader_.version_ = version;
   vheader_.services_ = services;
   vheader_.timestamp_ = timestamp;

   vheader_.addr_recv_.setIPv4(services, recvaddr);
   vheader_.addr_from_.setIPv4(services, fromaddr);

   auto&& randombytes = SecureBinaryData().GenerateRandom(8);
   vheader_.nonce_ = *(uint64_t*)randombytes.getPtr();
}

////////////////////////////////////////////////////////////////////////////////
void Payload_Ping::deserialize(uint8_t* dataptr, size_t len)
{
   if (len == 0)
      nonce_ = UINT64_MAX;
   else if (len != 8)
      throw BitcoinMessageDeserError("invalid ping payload len");
   else
      nonce_ = *(uint64_t*)dataptr;
}

////////////////////////////////////////////////////////////////////////////////
size_t Payload_Ping::serialize_inner(uint8_t* dataptr) const
{
   if (nonce_ == UINT64_MAX)
      return 0;
   
   if (dataptr == nullptr)
      return 8;

   uint64_t* ptr = (uint64_t*)dataptr;
   *ptr = nonce_;

   return 8;
}

////////////////////////////////////////////////////////////////////////////////
void Payload_Pong::deserialize(uint8_t* dataptr, size_t len)
{
   if (len != 8)
      throw BitcoinMessageDeserError("invalid pong payload len");

   nonce_ = *(uint64_t*)dataptr;
}

////////////////////////////////////////////////////////////////////////////////
size_t Payload_Pong::serialize_inner(uint8_t* dataptr) const
{
   if (nonce_ == UINT64_MAX)
      return 0;

   if (dataptr == nullptr)
      return 8;

   uint64_t* ptr = (uint64_t*)dataptr;
   *ptr = nonce_;

   return 8;
}

////////////////////////////////////////////////////////////////////////////////
void Payload_Inv::deserialize(uint8_t* dataptr, size_t len)
{
   uint64_t invCount;
   auto varintlen = get_varint(invCount, dataptr, len);

   if (invCount > INV_MAX)
      throw BitcoinMessageDeserError("inv count > INV_MAX");

   invVector_.resize(invCount);

   auto ptr = dataptr + varintlen;
   auto remaining = len - varintlen;

   for (auto& entry : invVector_)
   {
      if (remaining < INV_ENTRY_LEN)
         throw BitcoinMessageDeserError("inv deser size mismatch");

      auto entrytype = *(uint32_t*)(ptr);
      if (entrytype > 3)
         throw BitcoinMessageDeserError("invalid inv entry type");

      entry.invtype_ = (InvType)entrytype;
      memcpy(entry.hash, ptr + 4, 32);

      remaining -= INV_ENTRY_LEN;
      ptr += INV_ENTRY_LEN;
   }
}

////////////////////////////////////////////////////////////////////////////////
size_t Payload_Tx::serialize_inner(uint8_t* dataptr) const
{
   if (dataptr == nullptr)
      return rawTx_.size();

   memcpy(dataptr, &rawTx_[0], rawTx_.size());
   return rawTx_.size();
}

////////////////////////////////////////////////////////////////////////////////
void Payload_Tx::deserialize(uint8_t* dataptr, size_t len)
{
   rawTx_.resize(len);
   memcpy(&rawTx_[0], dataptr, len);
}

////////////////////////////////////////////////////////////////////////////////
size_t Payload_Inv::serialize_inner(uint8_t* dataptr) const
{
   if (dataptr == nullptr)
   {
      auto invcount = invVector_.size();
      auto varintlen = get_varint_len(invcount);

      return invcount * INV_ENTRY_LEN + varintlen;
   }

   auto invcount = invVector_.size();
   vector<uint8_t> varint;
   auto varintlen = make_varint(invcount, varint);

   memcpy(dataptr, &varint[0], varintlen);
   dataptr += varintlen;

   for (auto& entry : invVector_)
   {
      auto intptr = (uint32_t*)dataptr;
      *intptr = (uint32_t)entry.invtype_;
      memcpy(dataptr + 4, entry.hash, 32);

      dataptr += 36;
   }

   return varintlen + invcount * INV_ENTRY_LEN;
}

////////////////////////////////////////////////////////////////////////////////
void Payload_GetData::deserialize(uint8_t* dataptr, size_t len)
{
   uint64_t invCount;
   auto varintlen = get_varint(invCount, dataptr, len);

   if (invCount > INV_MAX)
      throw BitcoinMessageDeserError("inv count > INV_MAX");

   invVector_.resize(invCount);

   auto ptr = dataptr + varintlen;
   auto remaining = len - varintlen;

   for (auto& entry : invVector_)
   {
      if (remaining < INV_ENTRY_LEN)
         throw BitcoinMessageDeserError("inv deser size mismatch");

      auto entrytype = *(uint32_t*)(ptr);
      if (entrytype > 3)
         throw BitcoinMessageDeserError("invalid inv entry type");

      entry.invtype_ = (InvType)entrytype;
      memcpy(entry.hash, ptr + 4, 32);

      remaining -= INV_ENTRY_LEN;
      ptr += INV_ENTRY_LEN;
   }
}

////////////////////////////////////////////////////////////////////////////////
size_t Payload_GetData::serialize_inner(uint8_t* dataptr) const
{
   if (dataptr == nullptr)
   {
      auto invcount = invVector_.size();
      auto varintlen = get_varint_len(invcount);

      return invcount * INV_ENTRY_LEN + varintlen;
   }

   auto invcount = invVector_.size();
   vector<uint8_t> varint;
   auto varintlen = make_varint(invcount, varint);

   memcpy(dataptr, &varint[0], varintlen);
   dataptr += varintlen;

   for (auto& entry : invVector_)
   {
      auto intptr = (uint32_t*)dataptr;
      *intptr = (uint32_t)entry.invtype_;
      memcpy(dataptr + 4, entry.hash, 32);

      dataptr += 36;
   }

   return varintlen + invcount * INV_ENTRY_LEN;
}

////////////////////////////////////////////////////////////////////////////////
////
//// BitcoinP2P
////
////////////////////////////////////////////////////////////////////////////////
BitcoinP2P::BitcoinP2P(const string& addrV4, const string& port,
   uint32_t magicword) :
   binSocket_(addrV4, port), magic_word_(magicword)
{
   run_.store(true, memory_order_relaxed);

}

////////////////////////////////////////////////////////////////////////////////
BitcoinP2P::~BitcoinP2P()
{
   //TODO: kill connectLoop first
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::connectToNode(bool async)
{
   unique_lock<mutex> lock(connectMutex_, defer_lock);

   if (!lock.try_lock()) //return if another thread is already here
      throw SocketError("another connect attempt is underway");

   connectedPromise_ = unique_ptr<promise<bool>>(new promise<bool>());
   auto connectedFuture = connectedPromise_->get_future();

   auto connectLambda = [this](void)->void
   {
      this->connectLoop();
   };

   thread connectthread(connectLambda);
   if (connectthread.joinable())
      connectthread.detach();

   if (async)
      return;

   connectedFuture.get();

   if (select_except_ != nullptr)
      rethrow_exception(select_except_);

   if (process_except_ != nullptr)
      rethrow_exception(process_except_);
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::connectLoop(void)
{
   size_t waitBeforeReconnect = 0;
   promise<bool> shutdownPromise;
   shutdownFuture_ = shutdownPromise.get_future();

   while (run_.load(memory_order_relaxed))
   {
      //clean up stacks
      dataStack_ = make_shared<BlockingStack<vector<uint8_t>>>();

      verackPromise_ = make_unique<promise<bool>>();
      auto verackFuture = verackPromise_->get_future();

      while (run_.load(memory_order_relaxed))
      {
         if (binSocket_.openSocket(false))
            break;

         if (waitBeforeReconnect < 5000)
            waitBeforeReconnect += RECONNECT_INCREMENT_MS;

         this_thread::sleep_for(chrono::milliseconds(waitBeforeReconnect));
      }

      auto processThread = [this](void)->void
      {
         try
         {
            this->processDataStackThread();
         }
         catch (...)
         {
            this->process_except_ = current_exception();
         }
      };

      pollSocketThread();
      thread processThr(processThread);

      //send version payload
      Payload_Version version;
      auto timestamp = getTimeStamp();

      struct sockaddr clientsocketaddr;

      try
      {
         //send version
         if (binSocket_.getSocketName(clientsocketaddr) != 0)
            throw SocketError("failed to get client sockaddr");

         // Services, for future extensibility
         uint32_t services = 0 | NODE_WITNESS;

         version.setVersionHeaderIPv4(70012, services, timestamp,
            node_addr_, clientsocketaddr);

         version.userAgent_ = "Armory:0.95";
         version.startHeight_ = -1;

         sendMessage(move(version));

         //wait on verack
         verackFuture.get();
         verackPromise_.reset();
         LOGINFO << "Connected to Bitcoin node";

         //signal calling thread
         connectedPromise_->set_value(true);
         waitBeforeReconnect = 0;

      }
      catch (...)
      {
         waitBeforeReconnect += RECONNECT_INCREMENT_MS;
         this_thread::sleep_for(chrono::milliseconds(waitBeforeReconnect));
      }

      //wait on threads
      if (processThr.joinable())
         processThr.join();
      
      //close socket to guarantee select returns
      if (binSocket_.isValid())
         binSocket_.closeSocket();

      LOGINFO << "Disconnected from Bitcoin node";
   }

   shutdownPromise.set_value(true);
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::pollSocketThread()
{
   unique_lock<mutex> lock(pollMutex_, defer_lock);

   if (!lock.try_lock())
      throw SocketError("another poll thread is already running");

   auto dataStack = dataStack_;

   auto callback = [dataStack](
      vector<uint8_t> socketdata, exception_ptr ePtr)->bool
   {
      if (ePtr == nullptr && socketdata.size() > 0)
      {
         dataStack->push_back(move(socketdata));
         return false;
      }

      dataStack->terminate();
      return true;
   };

   binSocket_.readFromSocket(callback);
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::processDataStackThread()
{
   try
   {
      while (1)
      {
         auto&& data = dataStack_->pop_front();
         auto&& payload = Payload::deserialize(data, magic_word_);

         processPayload(move(payload));
      }
   }
   catch (...)
   {
      if (verackPromise_ == nullptr)
         return;

      exception_ptr eptr = current_exception();
      verackPromise_->set_exception(eptr);
   }
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::processPayload(vector<unique_ptr<Payload>> payloadVec)
{
   for (auto&& payload : payloadVec)
   {
      switch (payload->type())
      {
      case Payload_version:
         checkServices(move(payload));
         returnVerack();
         break;

      case Payload_verack:
         gotVerack();
         break;

      case Payload_ping:
         replyPong(move(payload));
         break;

      case Payload_inv:
         processInv(move(payload));
         break;

      case Payload_getdata:
         processGetData(move(payload));
         break;

      case Payload_tx:
         processGetTx(move(payload));
         break;

      default:
         continue;
      }
   }
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::checkServices(unique_ptr<Payload> payload)
{
   Payload_Version* pver = (Payload_Version*)payload.get();

   if(pver->vheader_.services_ & NODE_WITNESS)
      PEER_USES_WITNESS = true;
   else
      PEER_USES_WITNESS = false;
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::gotVerack(void)
{
   if (verackPromise_ == nullptr)
      return;

   try
   {
      verackPromise_->set_value(true);
   }
   catch (future_error&)
   {
      //already set or no shared state, move on
   }
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::returnVerack(void)
{
   Payload_Verack verack;
   sendMessage(move(verack));
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::replyPong(unique_ptr<Payload> payload)
{
   Payload_Ping* pping = (Payload_Ping*)payload.get();
   Payload_Pong ppong;

   ppong.nonce_ = pping->nonce_;
   sendMessage(move(ppong));
}


////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::processInv(unique_ptr<Payload> payload)
{
   Payload_Inv* invptr = (Payload_Inv*)payload.get();

   //order entries by type
   map<InvType, vector<InvEntry>> orderedEntries;
   
   for (auto& entry : invptr->invVector_)
   {
      auto& invvec = orderedEntries[entry.invtype_];
      invvec.push_back(move(entry));
   }

   //process them
   for (auto& entryVec : orderedEntries)
   {
      switch (entryVec.first)
      {
      case Inv_Msg_Block:
         processInvBlock(move(entryVec.second));
         break;

      case Inv_Msg_Tx:
         processInvTx(move(entryVec.second));
         break;

      default:
         continue;
      }
   }
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::processInvBlock(vector<InvEntry> invVec)
{
   vector<function<void(const vector<InvEntry>&)>> callbacksVec;
   try
   {
      while (1)
      {
         auto&& blockLambda = invBlockLambdas_.pop_front();
         callbacksVec.push_back(move(blockLambda));
      }
   }
   catch (IsEmpty&)
   {}

   for (auto& callback : callbacksVec)
      callback(invVec);
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::processInvTx(vector<InvEntry> invVec)
{
   invTxLambda_(invVec);
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::processGetData(unique_ptr<Payload> payload)
{
   Payload_GetData payloadgetdata = move(*(Payload_GetData*)payload.release());

   auto& invvector = payloadgetdata.getInvVector();
   auto getdatamap = getDataPayloadMap_.get();

   for (auto& entry : invvector)
   {
      BinaryDataRef bdr(entry.hash, 32);

      auto payloadIter = getdatamap->find(bdr);
      if (payloadIter == getdatamap->end())
         continue;

      if(payloadIter->second.payload_->type() != entry.invtype_)
         continue;

      auto&& payload = *payloadIter->second.payload_.get();
      sendMessage(move(payload));
      try
      {
         payloadIter->second.promise_->set_value(true);
      }
      catch (future_error&)
      {
         //do nothing
      }
   }
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::processGetTx(unique_ptr<Payload> payload)
{
   if (payload->type() != Payload_tx)
   {
      LOGERR << "processGetTx: expected payload type tx type, got " <<
         payload->typeStr() << " instead";
      return;
   }

   shared_ptr<Payload> payload_sptr(move(payload));
   auto payloadtx = dynamic_pointer_cast<Payload_Tx>(payload_sptr);
   if (payloadtx->getSize() == 0)
   {
      LOGERR << "empty rawtx";
      return;
   }

   auto& txHash = payloadtx->getHash256();

   {
      auto gettxcallbackmap = getTxCallbackMap_.get();

      auto callbackIter = gettxcallbackmap->find(txHash);
      if (callbackIter == gettxcallbackmap->end())
         return;
      
      callbackIter->second(move(payloadtx));
   }

   getTxCallbackMap_.erase(txHash);
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::sendMessage(Payload&& payload)
{
   auto&& msg = payload.serialize(magic_word_);

   unique_lock<mutex> lock(writeMutex_);
   binSocket_.writeToSocket(&msg[0], msg.size());
}

////////////////////////////////////////////////////////////////////////////////
int64_t BitcoinP2P::getTimeStamp() const
{
   return (int64_t)time(0);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<Payload_Tx> BitcoinP2P::getTx(
   const InvEntry& entry, uint32_t timeout)
{
   //blocks until data is received or timeout expires
   if (entry.invtype_ != Inv_Msg_Tx && entry.invtype_ != Inv_Msg_Witness_Tx)
      throw GetDataException("entry type isnt Inv_Msg_Tx");

   BinaryDataRef txHash(entry.hash, 32);

   //use a shared_ptr until we start using a C++14 compiler for
   //lambda generalized capture
   auto gotDataPromise = make_shared<promise<shared_ptr<Payload_Tx>>>();
   auto&& gotDataFuture = gotDataPromise->get_future().share();

   auto waitOnDataCallback = 
      [gotDataPromise](shared_ptr<Payload_Tx> payload)->void
   {
      try
      {
         gotDataPromise->set_value(payload);
      }
      catch (future_error&)
      {
         //do nothing
      }
   };

   //register callback
   registerGetTxCallback(txHash, move(waitOnDataCallback));

   //send message
   Payload_GetData payload(entry);
   sendMessage(move(payload));

   //wait on promise
   if (timeout == 0)
   {
      //wait undefinitely if timeout is 0
      return gotDataFuture.get();
   }
   else
   {
      auto&& status = gotDataFuture.wait_for(chrono::seconds(timeout));
      if (status != future_status::ready)
      {
         //unregister callback
         unregisterGetTxCallback(txHash);

         //throw
         throw GetDataException("operation timed out");
      }

      return gotDataFuture.get();
   }
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::registerGetTxCallback(
   const BinaryDataRef& hashRef, getTxCallback callback)
{
   getTxCallbackMap_.insert(move(make_pair(
      hashRef, callback)));
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::unregisterGetTxCallback(
   const BinaryDataRef& hashRef)
{
   getTxCallbackMap_.erase(hashRef);
}

////////////////////////////////////////////////////////////////////////////////
void BitcoinP2P::shutdown()
{
   run_.store(false, memory_order_relaxed);
   binSocket_.closeSocket();

   //wait until connect loop exists
   shutdownFuture_.wait();

   //clean up remaining lambdas
   vector<InvEntry> ieVec;
   InvEntry entry;
   entry.invtype_ = Inv_Terminate;
   ieVec.push_back(entry);

   processInvBlock(ieVec);
}