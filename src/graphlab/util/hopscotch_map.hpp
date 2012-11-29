/**  
 * Copyright (c) 2009 Carnegie Mellon University. 
 *
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

#ifndef GRAPHLAB_UTIL_HOPSCOTCH_HASH_HPP 
#define GRAPHLAB_UTIL_HOPSCOTCH_HASH_HPP 

#include <graphlab/util/hopscotch_table.hpp>
#include <graphlab/parallel/pthread_tools.hpp>
#include <graphlab/parallel/atomic.hpp>

#include <graphlab/serialization/serialization_includes.hpp>


#if __cplusplus < 201103L 
#include <boost/functional/hash.hpp>
#define _HOPSCOTCH_MAP_DEFAULT_HASH boost::hash<Key>

#else
#define _HOPSCOTCH_MAP_DEFAULT_HASH std::hash<Key>
#endif


namespace graphlab {



  /**
   * A hopscotch hash map. More or less similar
   * interface as boost::unordered_map, not necessarily
   * entirely STL compliant.
   * Really should only be used to store small keys and trivial values.
   *
   * \tparam Key The key of the map
   * \tparam Value The value to store for each key
   * \tparam Synchronized Defaults to True. If True, locking is used to ensure
   *                      safe reads and writes to the hash table.
   *                      Even under "Synchronized", the only operations
   *                      which are safe for parallel access are all functions 
   *                      suffixed with "sync".
   * \tparam Hash The hash functor type. Defaults to std::hash<Key> if C++11 is
   *              available. Otherwise defaults to boost::hash<Key>
   * \tparam KeyEqual The functor used to identify object equality. Defaults to
   *                  std::equal_to<Key>
   */
  template <typename Key, 
            typename Value, 
            bool Synchronized = true,
            typename Hash = _HOPSCOTCH_MAP_DEFAULT_HASH,
            typename KeyEqual = std::equal_to<Key> >
  class hopscotch_map {

  public:
    // public typedefs
    typedef Key                                      key_type;
    typedef std::pair<Key, Value>                    value_type;
    typedef Value                                    mapped_type;
    typedef size_t                                   size_type;
    typedef Hash                                     hasher;
    typedef KeyEqual equality_function;
    typedef value_type* pointer;
    typedef value_type& reference;
    typedef const value_type* const_pointer;
    typedef const value_type& const_reference;

    
    typedef std::pair<Key, Value>                    storage_type;

    struct hash_redirect{
      Hash hashfun;
      hash_redirect(Hash h): hashfun(h) { }
      size_t operator()(const storage_type& v) const {
        return hashfun(v.first);
      }
    };
    struct key_equal_redirect{
      KeyEqual keyeq;
      key_equal_redirect(KeyEqual k): keyeq(k) { }
      bool operator()(const storage_type& v, const storage_type& v2) const {
        return keyeq(v.first, v2.first);
      }
    };
 
    typedef hopscotch_table<storage_type, 
                            Synchronized,
                            hash_redirect, 
                            key_equal_redirect> container_type;

    typedef typename container_type::iterator iterator;
    typedef typename container_type::const_iterator const_iterator;

  private:


    // The primary storage. Used by all sequential accessors.
    container_type* container;
    spinrwlock2 lock;

    // the hash function to use. hashes a pair<key, value> to hash(key)
    hash_redirect hashfun;

    // the equality function to use. Tests equality on only the first 
    // element of the pair
    key_equal_redirect equalfun;

    container_type* create_new_container(size_t size) {
      return new container_type(size, hashfun, equalfun);
    }

    void destroy_all() {
      delete container;
      container = NULL;
    } 

    // rehashes the hash table to one which is double the size
    container_type* rehash_to_new_container(size_t newsize = (size_t)(-1)) {
      /*
         std::cerr << "Rehash at " << container->size() << "/" 
         << container->capacity() << ": " 
         << container->load_factor() << std::endl;
       */
      // rehash
      if (newsize == (size_t)(-1)) newsize = container->size() * 2;
      container_type* newcontainer = create_new_container(newsize);
      const_iterator citer = begin();
      while (citer != end()) {
        assert(newcontainer->insert(*citer) != newcontainer->end());
        ++citer;
      }
      return newcontainer;
    }

    // Inserts a value into the hash table. This does not check
    // if the key already exists, and may produce duplicate values.
    iterator do_insert(const value_type &v) {
      iterator iter = container->insert(v);

      if (iter != container->end()) {
          return iter;
      }
      else {
        container_type* newcontainer = rehash_to_new_container();
        iter = newcontainer->insert(v);
        assert(iter != newcontainer->end());
        std::swap(container, newcontainer);
        delete newcontainer;
        return iter;
      }
    }

  public:

    hopscotch_map(Hash hashfun = Hash(),
                  KeyEqual equalfun = KeyEqual()): 
                            container(NULL), 
                            hashfun(hashfun), equalfun(equalfun) {
      container = create_new_container(32);
    }

    hopscotch_map(const hopscotch_map& h): 
                            hashfun(h.hashfun), equalfun(h.equalfun) {
      container = create_new_container(h.capacity());
      (*container) = *(h.container);
    }

    // only increases
    void rehash(size_t s) {
      if (s > capacity()) {
        container_type* newcontainer = rehash_to_new_container(s);
        std::swap(container, newcontainer);
        delete newcontainer;
      }
    }

    ~hopscotch_map() {
      destroy_all();
    }

    hasher hash_function() const {
      return hashfun.hashfun;
    }

    KeyEqual key_eq() const {
      return equalfun.equalfun;
    }

    hopscotch_map& operator=(const hopscotch_map& other) {
      (*container) = *(other.container);
      hashfun = other.hashfun;
      equalfun = other.equalfun;
      return *this;
    }
  
    size_type size() const {
      return container->size();
    }

    iterator begin() {
      return container->begin();
    }

    iterator end() {
      return container->end();
    }


    const_iterator begin() const {
      return container->begin();
    }

    const_iterator end() const {
      return container->end();
    }

     
    std::pair<iterator, bool> insert(const value_type& v) {
      iterator i = find(v.first);
      if (i != end()) return std::make_pair(i, false);
      else return std::make_pair(do_insert(v), true);
    }
     

    iterator insert(const_iterator hint, const value_type& v) {
      return insert(v).first;
    }

    iterator find(key_type const& k) {
      value_type v(k, mapped_type());
      return container->find(v);
    }

    const_iterator find(key_type const& k) const {
      value_type v(k, mapped_type());
      return container->find(v);
    }

    size_t count(key_type const& k) const {
      value_type v(k, mapped_type());
      return container->count(v);
    }

  
    bool erase(iterator iter) {
      return container->erase(iter);
    }

    bool erase(key_type const& k) {
      value_type v(k, mapped_type());
      return container->erase(v);
    }

    void swap(hopscotch_map& other) {
      std::swap(container, other.container);
      std::swap(hashfun, other.hashfun);
      std::swap(equalfun, other.equalfun);
    }
  
    mapped_type& operator[](const key_type& i) {
      iterator iter = find(i);
      value_type tmp(i, mapped_type());
      if (iter == end()) iter = do_insert(tmp);
      return iter->second;
    }

    void clear() {
      destroy_all();
      container = create_new_container(128);
    }
    
    
    size_t capacity() const {
      return container->capacity();
    }


    float load_factor() const {
      return container->load_factor();
    }

    void save(oarchive &oarc) const {
      oarc << size() << capacity();
      const_iterator iter = begin();
      while (iter != end()) {
        oarc << (*iter);
        ++iter;
      }
    }


    void load(iarchive &iarc) {
      size_t s, c;
      iarc >> s >> c;
      if (capacity() != c) {
        destroy_all();
        container = create_new_container(c);
      }
      else {
        container->clear(); 
      }
      for (size_t i = 0;i < s; ++i) {
        value_type v;
        iarc >> v;
        insert(v);
      }
    }

    void put_sync(const value_type &v) {
      if (!Synchronized) {
        (*this)[v.first] = v.second;
        return;
      }
      lock.readlock();
      // try to insert into the container
      if (container->put_sync(v)) {
        // success!
        lock.rdunlock();
      }
      else {
        // fail!
        // I may need to rehash
        lock.rdunlock();
        // lets get an exclusive lock and try again
        lock.writelock();
        // I now have exclusive, I can use the unsynchronized accessors
        if (container->insert(v) != container->end()) {
          // success now
          lock.wrunlock();
        }
        else {
          // rehash
          container_type* newcontainer = rehash_to_new_container();
          // we much succeed now!
          assert(newcontainer->insert(v) != newcontainer->end());
          std::swap(container, newcontainer);
          lock.wrunlock();
          delete newcontainer;
        }
      }
    }

    void put_sync(const Key& k, const Value& v) {
      put_sync(std::make_pair(k, v)); 
    } 

    std::pair<bool, Value> get_sync(const Key& k) const {
      if (!Synchronized) {
        const_iterator iter = find(k);
        return std::make_pair(iter == end(), iter->second);
      }

      lock.readlock();
      std::pair<bool, value_type> v = 
                container->get_sync(std::make_pair(k, mapped_type()));
      lock.rdunlock();
      return std::make_pair(v.first, v.second.second);
    }      

    bool erase_sync(const Key& k) {
      if (!Synchronized) return erase(k);
      lock.readlock();
      bool ret = container->erase_sync(k);
      lock.rdunlock();
      return ret;
    }      

  }; 

}; // end of graphlab namespace

#endif
