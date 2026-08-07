// atsc3_service_guide.h — GNU Radio ATSC 3.0 Service Guide Block
//
// Message-only block for displaying service catalog
// Input: Message port "catalog" (receives updates from route_parser)
// Output: Message port "services" (JSON-formatted service list)

#ifndef INCLUDED_ATSC3_SERVICE_GUIDE_H
#define INCLUDED_ATSC3_SERVICE_GUIDE_H

#include <gnuradio/block.h>

namespace gr {
namespace atsc3 {

class service_guide : virtual public gr::block {
public:
    typedef std::shared_ptr<service_guide> sptr;

    /*!
     * \brief Create ATSC 3.0 service guide block
     * \return Shared pointer to new instance
     */
    static sptr make();

    /*!
     * \brief Get number of services
     */
    virtual size_t get_service_count() const = 0;

    /*!
     * \brief Get service info as JSON string
     * \param index Service index
     */
    virtual std::string get_service_json(size_t index) const = 0;

    /*!
     * \brief Get all services as JSON string
     */
    virtual std::string get_all_services_json() const = 0;
};

}  // namespace atsc3
}  // namespace gr

#endif /* INCLUDED_ATSC3_SERVICE_GUIDE_H */
