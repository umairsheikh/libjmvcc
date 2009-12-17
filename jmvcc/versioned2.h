/* versioned2.h                                                    -*- C++ -*-
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Turns a normal object into a versioned one.  This is a lock-free version.
*/

#ifndef __jmvcc__versioned2_h__
#define __jmvcc__versioned2_h__

#include "utils/circular_buffer.h"

namespace JMVCC {


/*****************************************************************************/
/* VERSIONED                                                                 */
/*****************************************************************************/

/** This template takes an underlying type and turns it into a versioned
    object.  It's used for simple objects where a new copy of the object
    can be stored for each version.

    For more complicated cases (for example, where a lot of the state
    can be shared between an old and a new version), the object should
    derive directly from Versioned_Object instead.
*/

template<typename T>
struct Versioned2 : public Versioned_Object {
    typedef ACE_Mutex Mutex;
    
    explicit Versioned2(const T & val = T())
    {
        data = new_data(val);
    }

    ~Versioned2()
    {
        delete_data(const_cast<Data *>(data));
    }

    // Client interface.  Just two methods to get at the current value.
    T & mutate()
    {
        if (!current_trans) no_transaction_exception(this);
        T * local = current_trans->local_value<T>(this);

        if (!local) {
            T value = data->value_at_epoch(current_trans->epoch());
            local = current_trans->local_value<T>(this, value);
            
            if (!local)
                throw Exception("mutate(): no local was created");
        }
        
        return *local;
    }

    void write(const T & val)
    {
        mutate() = val;
    }
    
    const T read() const
    {
        if (!current_trans) return data->value_at_epoch(get_current_epoch());
        const T * val = current_trans->local_value<T>(this);
        
        if (val) return *val;
        
        return data->value_at_epoch(current_trans->epoch());
    }

    size_t history_size() const { return data->size() - 1; }

private:
    // This structure provides a list of values.  Each one is tagged with the
    // earliest epoch in which it is valid.  The latest epoch in which it is
    // valid + 1 is that of the next entry in the list; that in current has no
    // latest epoch.

    struct Entry {
        explicit Entry(Epoch valid_to = 0, const T & value = T())
            : valid_to(valid_to), value(value)
        {
        }

        Epoch valid_to;
        T value;
    };

    // Internal data object allocated
    struct Data {
        Data(size_t capacity)
            : capacity(capacity), first(0), last(0)
        {
        }

        Data(size_t capacity, const Data & old_data)
            : capacity(capacity), first(0), last(0)
        {
            for (unsigned i = 0;  i < old_data.size();  ++i)
                push_back(old_data.element(i));
        }

        uint32_t capacity;   // Number allocated
        uint32_t first;      // Index of first valid entry
        uint32_t last;       // Index of last valid entry
        Entry history[0];  // real ones are allocated after

        uint32_t size() const { return last - first; }

        ~Data()
        {
            for (unsigned i = 0;  i < size;  ++i)
                history[i].value.~T();
        }

        /// Return the value for the given epoch
        const T & value_at_epoch(Epoch epoch) const
        {
            for (int i = last - 1;  i > first;  --i) {
                Epoch valid_from = history[i - 1].valid_to;
                if (epoch >= valid_from)
                    return history[i].value;
            }
            
            return history[first].value;
        }
        
        Data * copy(size_t new_capacity)
        {
            if (new_capacity < size())
                throw Exception("new capacity is wrong");

            return new_data(*this, new_capacity);
        }

        Entry & front()
        {
            return history[first];
        }

        const Entry & front() const
        {
            return history[first];
        }

        void pop_front()
        {
            /* Need to:
               1.  Increment first
               2.  Set up the destructor for that element to be run for
                   garbage collection
            */
            if (size() < 2)
                throw Exception("attempt to pop last valid value off");
            first += 1;
        }

        void pop_back()
        {
            if (size() < 2)
                throw Exception("popping back last element");
            --last;
        }

        void push_back(const Entry & entry)
        {
            if (last == capacity)
                throw Exception("can't push back");
            new (&history[last].value) T(entry.value);
            history[last].valid_to = entry.valid_to;
            
            __sync_synchronize();

            ++last;
        }
        
        const Entry & back() const
        {
            return history[last - 1];
        }

        Entry & back()
        {
            return history[last - 1];
        }
        
        Entry & element(int index)
        {
            if (index < 0 || index >= size())
                throw Exception("invalid element");
            return history[first + index];
        }

        const Entry & element(int index) const
        {
            if (index < 0 || index >= size())
                throw Exception("invalid element");
            return history[first + index];
        }
    };
    
    // The single internal data member.  Updated atomically.
    Data * data;

    static void delete_data(Data * data)
    {
        // For the moment, we leak, until we get GC working
    }

    static Data * new_data(const T & val, size_t capacity = 1)
    {
        // TODO: exception safety...
        void * d = malloc(sizeof(Data) + capacity * sizeof(Entry));
        Data * d2 = new (d) Data(capacity);
        d2->push_back(Entry(0,  val));
        return d2;
    }

    static Data * new_data(const Data & old, size_t capacity)
    {
        // TODO: exception safety...
        void * d = malloc(sizeof(Data) + capacity * sizeof(Entry));
        Data * d2 = new (d) Data(capacity, old);
        return d2;
    }

    bool set_data(Data * new_data)
    {
        // For the moment, the commit lock is held when we update this, so
        // there is no possibility of conflict.  But if ever we decide to
        // allow for parallel commits, then we need to be more careful here
        // to do it atomically.
        Data * old_data = data;
        data = new_data;
        delete_data(old_data);
        return true;
    }
        
public:
    // Implement object interface

    virtual bool setup(Epoch old_epoch, Epoch new_epoch, void * new_value)
    {
        if (new_epoch != get_current_epoch() + 1)
            throw Exception("epochs out of order");

        Epoch valid_from = 0;
        if (data->size() > 2)
            valid_from = data->element(data->size() - 2).valid_to;

        if (valid_from > old_epoch)
            return false;  // something updated before us

        Data * new_data = data->copy(data->size() + 1);
        new_data->back().valid_to = new_epoch;
        new_data->push_back(Entry(0 /* valid_to */,
                                  *reinterpret_cast<T *>(new_value)));
        
        set_data(new_data);

        return true;
    }

    virtual void commit(Epoch new_epoch) throw ()
    {
        // Now that it's definitive, we have an older entry to clean up
        Epoch valid_from = 0;
        if (data->size() > 2)
            valid_from = data->element(data->size() - 2).valid_to;
        snapshot_info.register_cleanup(this, valid_from);
    }

    virtual void rollback(Epoch new_epoch, void * local_data) throw ()
    {
        data->pop_back();
    }

    virtual void cleanup(Epoch unused_epoch, Epoch trigger_epoch)
    {
        if (data->size() < 2)
            throw Exception("cleaning up with no values to clean up");

        if (unused_epoch < data->front().valid_to) {
            // Can be done atomically
            data->pop_front();
            return;
        }

        Data * data2 = new_data(data->size());
        
        // Copy them, skipping the one that matched
        

        // TODO: optimize
        Epoch valid_from = 0;
        bool found = false;
        for (unsigned i = data->first, e = data->last, j = 0; i != e;  ++i) {
            if (valid_from == unused_epoch) {
                if (found)
                    throw Exception("two with the same valid_from value");
                found = true;
                if (j != 0)
                    data2->history[j - 1].valid_to = data->history[i].valid_to;
            }
            else {
                // Copy element i to element j
                new (&data2->history[j].value) T(data->history[i].value);
                data2->history[j].valid_to = data->history[i].valid_to;
                ++j;
            }

            valid_from = data->history[i].valid_to;
        }

        if (found) {
            set_data(data2);
            return;
        }

        using namespace std;
        cerr << "----------- cleaning up didn't exist ---------" << endl;
        dump_unlocked();
        cerr << "unused_epoch = " << unused_epoch << endl;
        cerr << "trigger_epoch = " << trigger_epoch << endl;
        snapshot_info.dump();
        cerr << "----------- end cleaning up didn't exist ---------" << endl;
        
        throw Exception("attempt to clean up something that didn't exist");
    }
    
    virtual void rename_epoch(Epoch old_epoch, Epoch new_epoch) throw ()
    {
        throw Exception("versioned2: no renaming");
#if 0
        if (history.empty())
            throw Exception("renaming up with no values");
        
        if (old_epoch < history[0].valid_to) {
            // The last one doesn't have a valid_from, so we assume that it's
            // ok and leave it.
            return;
        }

        // TODO: optimize
        int i = 0;
        for (typename History::iterator
                 it = history.begin(),
                 end = history.end();
             it != end;  ++it, ++i) {
            
            if (it->valid_to == old_epoch) {
                if (i != 0 && boost::prior(it)->valid_to >= new_epoch)
                    throw Exception("new epoch not ordered with respect to "
                                    "old");
                if (i != history.size() - 1
                    && boost::next(it)->valid_to <= new_epoch)
                    throw Exception("new epoch not ordered with respect to "
                                    "old 2");
                
                it->valid_to = new_epoch;
                return;
            }
        }

        using namespace std;
        cerr << "---------------------" << endl;
        cerr << "old_epoch = " << old_epoch << endl;
        cerr << "new_epoch = " << new_epoch << endl;
        dump_unlocked();
        throw Exception("attempt to rename something that didn't exist");
#endif
    }

    virtual void dump(std::ostream & stream = std::cerr, int indent = 0) const
    {
        dump_itl(stream, indent);
    }

    virtual void dump_unlocked(std::ostream & stream = std::cerr,
                               int indent = 0) const
    {
        dump_itl(stream, indent);
    }

    void dump_itl(std::ostream & stream, int indent = 0) const
    {
        using namespace std;
        std::string s(indent, ' ');
        stream << s << "object at " << this << std::endl;
        stream << s << "history with " << data->size()
               << " values" << endl;
        for (unsigned i = 0;  i < data->size();  ++i) {
            const Entry & entry = data->element(i);
            stream << s << "  " << i << ": valid to "
                   << entry.valid_to;
            stream << " addr " << &entry.value;
            stream << " value " << entry.value;
            stream << endl;
        }
    }

    virtual std::string print_local_value(void * val) const
    {
        return ostream_format(*reinterpret_cast<T *>(val));
    }

    virtual void validate() const
    {
#if 0
        ssize_t e = 0;  // epoch we are up to
        
        for (unsigned i = 0;  i < history.size();  ++i) {
            Epoch e2 = history[i].valid_to;
            if (e2 > get_current_epoch() + 1) {
                using namespace std;
                cerr << "e = " << e << " e2 = " << e2 << endl;
                dump();
                cerr << "invalid current epoch" << endl;
                throw Exception("invalid current epoch");
            }
            if (e2 <= e) {
                using namespace std;
                cerr << "e = " << e << " e2 = " << e2 << endl;
                dump();
                cerr << "invalid epoch order" << endl;
                throw Exception("invalid epoch order");
            }
            e = e2;
        }
#endif
    }
};

} // namespace JMVCC


#endif /* __jmvcc__versioned2_h__ */