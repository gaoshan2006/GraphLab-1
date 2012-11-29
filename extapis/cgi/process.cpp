/**  
 * Copyright (c) 2009 Carnegie Mellon University. 
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://www.graphlab.ml.cmu.edu
 *
 */
 
/**
 * @file process.cpp
 * @author Jiunn Haur Lim <jiunnhal@cmu.edu>
 */
 
#include <fstream>
#include <sstream>
#include "process.hpp"
#include "json_message.hpp"

using namespace graphlab;
namespace io = boost::asio;

/////////////////////////////// INSTANCE MEMBERS ///////////////////////////////

process::
  process() : ios(), pout(ios), pin(ios) {
  if (executable.empty()) return;
  redirect_io();
  return;
}

process::~process(){
  // close if child is still alive
  json_invocation exit_message("exit");
  send(exit_message);
  if (pout.is_open()) pout.close();
  if (pin.is_open()) pin.close();
}

void process::redirect_io (){

  // prepare args
  char **cargs = new char*[args.size() + 1];
  unsigned i = 0;
  for (std::vector<std::string>::iterator it=args.begin(); it!=args.end(); ++it){
    cargs[i] = new char[it->length()+1];
    snprintf(cargs[i++], it->length()+1, "%s", it->c_str());
  }
  cargs[i] = NULL;

  int opipefd[2];             // dispatcher -> child
  int ipipefd[2];             // dispatcher <- child
  CHECK (!::pipe(opipefd));   // assert pipe created
  CHECK (!::pipe(ipipefd));
  
  pid_t pid = ::fork();
  CHECK (0 <= pid);           // assert child created
  
  if (0 == pid){              // child process
    CHECK (!::close(opipefd[1]));
    CHECK (!::close(ipipefd[0]));
    CHECK (0 <= ::dup2(opipefd[0], 0));
    CHECK (0 <= ::dup2(ipipefd[1], 1));
    CHECK (!::close(opipefd[0]));
    CHECK (!::close(ipipefd[1]));
    pid_t ppid = ::getppid();
    CHECK(!::setpgid(0, ppid));
    CHECK(0 <= ::execv(executable.c_str(), cargs));
  } /* child process goes no further than here */

  // parent process
  for (i=0; i<args.size(); i++) delete[] cargs[i];
  delete[] cargs;
  
  pout.assign(opipefd[1]);
  pin.assign(ipipefd[0]);
  CHECK (!::close(opipefd[0]));
  CHECK (!::close(ipipefd[1]));
  
}

std::size_t process::write(json_message& message) {
  
  if (!pout.is_open())
    logstream(LOG_FATAL) << "Pipe closed unexpectedly." << std::endl;

  io::streambuf buffer;
  std::ostream output(&buffer);
  output << message << std::flush;
  
  std::size_t bytes = buffer.size();
  try {
    // write deals with short counts
    std::ostringstream int_os;
    int_os << bytes << "\r\n";
    io::write(pout, io::buffer(int_os.str()));
    bytes = io::write(pout, buffer);
  }catch (boost::system::system_error e){
    throw e;  // TODO error handling
  }
  
  // Note to self: we don't want async_write here, because our purpose is to 
  // wait for child to fully receive the message and then wait for child to 
  // reply. Using async is just beating around the bush to write really complex
  // code with no additional benefits.
  
  return bytes;
  
}

json_message& process::read(json_message& message){

  if (!pin.is_open())
    logstream(LOG_FATAL) << "Pipe closed unexpectedly." << std::endl;
  
  // get first line
  io::streambuf fl_buffer;
  std::size_t fl_count = io::read_until(pin, fl_buffer, '\n');
  std::size_t leftover = fl_buffer.in_avail() - fl_count;
  
  // extract number of bytes
  std::istream fs(&fl_buffer);
  std::size_t bytes = 0; fs >> bytes;
  std::string line; std::getline(fs, line);

  // prepare buffer and init w. leftovers
  json_message::byte data[bytes+1]; // plus 1 for null-terminator (temp)
  memset(data, 0, bytes+1);
  fl_buffer.sgetn(data, leftover);

  if (leftover < bytes) try {
    io::read(pin, io::buffer(data+leftover, bytes-leftover), io::transfer_all());
  }catch (boost::system::system_error e){
    // TODO: handle error
    logstream(LOG_ERROR) << e.what() << std::endl;
  }
  
  logstream(LOG_INFO) << "json: " << data << std::endl;

  message.parse(data, bytes);
  return message;
  
}

std::size_t process::send(json_message& message){
  return write(message);
}

json_message& process::receive(json_message& message){
  return read(message);
}

///////////////////////////////// CLASS MEMBERS ////////////////////////////////

const size_t process::PROC_ID = 2;
std::string process::executable = "";
std::vector<std::string> process::args;

void process::set_executable(const std::string path){
  executable = path;
}

void process::add_arg(const std::string& arg){
  args.push_back(arg);
}

process& process::get_process(){
       
  if (!thread::contains(PROC_ID)) {
    // store process in thread-local storage
    process *p = new process;
    thread::get_local(PROC_ID) = p;
    thread::set_thread_destroy_callback(detach_process);   
  }
  
  // return the process associated with the current thread
  process *p =  thread::get_local(PROC_ID).as<process *>();
  return *p;
  
}

void process::detach_process(){
  if (!thread::contains(PROC_ID)) return; // nothing to do
  process *p =  thread::get_local(PROC_ID).as<process *>();
  if (p) delete p;
  thread::get_local(PROC_ID) = NULL;
}
