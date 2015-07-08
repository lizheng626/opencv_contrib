#include "precomp.hpp"
#include <set>
#include <algorithm>
#include <iostream>
#include <sstream>

using namespace cv;
using namespace cv::dnn;

using std::vector;
using std::map;
using std::make_pair;
using std::set;

namespace cv
{
namespace dnn
{

template<typename T>
String toString(const T &v)
{
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

struct LayerPin
{
    int lid;
    int oid;

    LayerPin(int layerId = -1, int outputId = -1)
        : lid(layerId), oid(outputId) {}
    
    bool valid() const
    {
        return (lid >= 0 && oid >= 0);
    }

    bool equal(const LayerPin &r) const
    {
        return (lid == r.lid && oid == r.oid);
    }
};

struct LayerData
{
    LayerData() {}
    LayerData(int _id, const String &_name, const String &_type, LayerParams &_params)
        : id(_id), name(_name), type(_type), params(_params) {}

    int id;
    String name;
    String type;
    LayerParams params;

    std::vector<LayerPin> inputBlobsId;
    std::set<int> inputLayersId;
    std::set<int> requiredOutputs;

    Ptr<Layer> layerInstance;
    std::vector<Blob> outputBlobs;
    std::vector<Blob*> inputBlobs;

    int flag;

    Ptr<Layer> getLayerInstance()
    {
        if (layerInstance)
            return layerInstance;

        layerInstance = LayerRegister::createLayerInstance(type, params);
        if (!layerInstance)
        {
            CV_Error(Error::StsError, "Can't create layer \"" + name + "\" of type \"" + type + "\"");
        }

        return layerInstance;
    }
};

//fake layer containing network input blobs
struct NetInputLayer : public Layer
{
    void allocate(const std::vector<Blob*>&, std::vector<Blob>&) {}
    void forward(std::vector<Blob*>&, std::vector<Blob>&) {}

    int outputNameToIndex(String tgtName)
    {
        int idx = (int)(std::find(outNames.begin(), outNames.end(), tgtName) - outNames.begin());
        return (idx < (int)outNames.size()) ? idx : -1;
    }

    void setNames(const std::vector<String> &names)
    {
        outNames.assign(names.begin(), names.end());
    }

private:
    std::vector<String> outNames;
};

struct Net::Impl
{
    Impl()
    {   
        //allocate fake net input layer
        netInputLayer = Ptr<NetInputLayer>(new NetInputLayer());
        LayerData &inpl = layers.insert( make_pair(0, LayerData()) ).first->second;
        inpl.id = 0;
        inpl.name = "_input";
        inpl.type = "__NetInputLayer__";
        inpl.layerInstance = netInputLayer;

        lastLayerId = 1;
        netWasAllocated = false;
    }

    Ptr<NetInputLayer> netInputLayer;
    std::vector<int> netOutputs;

    typedef std::map<int, LayerData> MapIdToLayerData;
    std::map<int, LayerData> layers;
    std::map<String, int> layerNameToId;

    int lastLayerId;

    bool netWasAllocated;

    void setUpNet()
    {
        if (!netWasAllocated)
        {
            allocateLayers();
            computeNetOutputLayers();

            netWasAllocated = true;
        }
    }

    int getLayerId(const String &layerName)
    {
        std::map<String, int>::iterator it = layerNameToId.find(layerName);
        return (it != layerNameToId.end()) ? it->second : -1;
    }

    int getLayerId(int id)
    {
        MapIdToLayerData::iterator it = layers.find(id);
        return (it != layers.end()) ? id : -1;
    }

    int getLayerId(DictValue &layerDesc)
    {
        if (layerDesc.isInt())
            return getLayerId(layerDesc.get<int>());
        else if (layerDesc.isString())
            return getLayerId(layerDesc.get<String>());

        CV_Assert(layerDesc.isInt() || layerDesc.isString());
        return -1;
    }

    String getLayerName(int id)
    {
        MapIdToLayerData::iterator it = layers.find(id);
        return (it != layers.end()) ? it->second.name : "(unknown layer)";
    }

    LayerData& getLayerData(int id)
    {
        MapIdToLayerData::iterator it = layers.find(id);
        
        if (it == layers.end())
            CV_Error(Error::StsError, "Layer with requested id=" + toString(id) + " not found");

        return it->second;
    }

    LayerData& getLayerData(const String &layerName)
    {
        int id = getLayerId(layerName);

        if (id < 0)
            CV_Error(Error::StsError, "Requsted layer \"" + layerName + "\" not found");

        return getLayerData(id);
    }

    LayerData& getLayerData(const DictValue &layerDesc)
    {
        if (layerDesc.isInt())
            return getLayerData(layerDesc.get<int>());
        else if (layerDesc.isString())
            return getLayerData(layerDesc.get<String>());

        CV_Assert(layerDesc.isInt() || layerDesc.isString());
        return *((LayerData*)NULL);
    }

    static void addLayerInput(LayerData &ld, int inNum, LayerPin from)
    {
        if ((int)ld.inputBlobsId.size() <= inNum)
        {
            ld.inputBlobsId.resize(inNum + 1);
        }
        else
        {
            LayerPin storedFrom = ld.inputBlobsId[inNum];
            if (storedFrom.valid() && !storedFrom.equal(from))
                CV_Error(Error::StsError, "Input #" + toString(inNum) + "of layer \"" + ld.name + "\" already was connected");
        }

        ld.inputBlobsId[inNum] = from;
    }

    static void splitPin(const String &pinAlias, String &layerName, String &outName)
    {
        size_t delimPos = pinAlias.find('.');
        layerName = pinAlias.substr(0, delimPos);
        outName = (delimPos == String::npos) ? String() : pinAlias.substr(delimPos + 1);
    }

    int resolvePinOutputName(LayerData &ld, const String &outName, bool isOutPin)
    {
        if (outName.empty())
            return 0;

        if (std::isdigit(outName[0]))
        {
            char *lastChar;
            long inum = std::strtol(outName.c_str(), &lastChar, 10);

            if (*lastChar == 0)
            {
                CV_Assert(inum == (int)inum);
                return (int)inum;
            }
        }

        if (isOutPin)
            return ld.getLayerInstance()->outputNameToIndex(outName);
        else
            return ld.getLayerInstance()->inputNameToIndex(outName);
    }

    LayerPin getPinByAlias(const String &pinAlias, bool isOutPin = true)
    {
        LayerPin pin;
        String layerName, outName;
        splitPin(pinAlias, layerName, outName);

        pin.lid = (layerName.empty()) ? 0 : getLayerId(layerName);

        if (pin.lid >= 0)
            pin.oid = resolvePinOutputName(getLayerData(pin.lid), outName, isOutPin);

        return pin;
    }

    void connect(int outLayerId, int outNum, int inLayerId, int inNum)
    {
        LayerData &ldOut = getLayerData(outLayerId);
        LayerData &ldInp = getLayerData(inLayerId);

        addLayerInput(ldInp, inNum, LayerPin(outLayerId, outNum));
        ldOut.requiredOutputs.insert(outNum);
    }

    void computeNetOutputLayers()
    {
        netOutputs.clear();

        MapIdToLayerData::iterator it;
        for (it = layers.begin(); it != layers.end(); it++)
        {
            int lid = it->first;
            LayerData &ld = it->second;

            if (ld.requiredOutputs.size() == 0)
                netOutputs.push_back(lid);
        }

        std::cout << "\nNet Outputs(" << netOutputs.size() << "):\n";
        for (size_t i = 0; i < netOutputs.size(); i++)
            std::cout << layers[netOutputs[i]].name << std::endl;
    }

    void allocateLayer(int lid)
    {
        LayerData &ld = layers[lid];

        //already allocated
        if (ld.flag)
            return;

        //allocate parents
        for (set<int>::iterator i = ld.inputLayersId.begin(); i != ld.inputLayersId.end(); i++)
            allocateLayer(*i);

        //bind inputs
        ld.inputBlobs.resize(ld.inputBlobsId.size());
        for (size_t i = 0; i < ld.inputBlobsId.size(); i++)
        {
            LayerPin from = ld.inputBlobsId[i];
            CV_Assert(from.valid());
            ld.inputBlobs[i] = &layers[from.lid].outputBlobs[from.oid];
        }

        //allocate layer
        ld.outputBlobs.resize(ld.requiredOutputs.size());
        ld.getLayerInstance()->allocate(ld.inputBlobs, ld.outputBlobs);

        ld.flag = 1;
    }

    void allocateLayers()
    {
        MapIdToLayerData::iterator it;
        for (it = layers.begin(); it != layers.end(); it++)
            it->second.flag = 0;

        for (it = layers.begin(); it != layers.end(); it++)
        {
            int lid = it->first;
            allocateLayer(lid);
        }
    }

    void forwardLayer(LayerData &ld, bool clearFlags = true)
    {
        if (clearFlags)
        {
            MapIdToLayerData::iterator it;
            for (it = layers.begin(); it != layers.end(); it++)
                it->second.flag = 0;
        }

        //already was forwarded
        if (ld.flag)
            return;

        //forward parents
        for (set<int>::iterator i = ld.inputLayersId.begin(); i != ld.inputLayersId.end(); i++)
        {
            forwardLayer(layers[*i], false);
        }

        //forward itself
        ld.layerInstance->forward(ld.inputBlobs, ld.outputBlobs);

        ld.flag = 1;
    }

    void forwardAll()
    {
        MapIdToLayerData::iterator it;
        for (it = layers.begin(); it != layers.end(); it++)
            it->second.flag = 0;

        for (it = layers.begin(); it != layers.end(); it++)
            forwardLayer(it->second, false);
    }
};

Net::Net() : impl(new Net::Impl)
{

}

Net::~Net()
{

}

int Net::addLayer(const String &name, const String &type, LayerParams &params)
{
    if (name.find('.') != String::npos)
    {
        CV_Error(Error::StsBadArg, "Added layer name \"" + name + "\" should not contain dot symbol");
        return -1;
    }

    if (impl->getLayerId(name) >= 0)
    {
        CV_Error(Error::StsBadArg, "Layer \"" + name + "\" already into net");
        return -1;
    }

    int id = ++impl->lastLayerId;
    impl->layerNameToId.insert(std::make_pair(name, id));
    impl->layers.insert(std::make_pair(id, LayerData(id, name, type, params)));

    return id;
}

void Net::connect(int outLayerId, int outNum, int inLayerId, int inNum)
{
    impl->connect(outLayerId, outNum, inLayerId, inNum);
}

void Net::connect(String _outPin, String _inPin)
{
    LayerPin outPin = impl->getPinByAlias(_outPin);
    LayerPin inpPin = impl->getPinByAlias(_inPin);

    CV_Assert(outPin.valid() && inpPin.valid());

    impl->connect(outPin.lid, outPin.oid, inpPin.lid, inpPin.oid);
}

void Net::forward()
{
    impl->setUpNet();
    impl->forwardAll();
}

void Net::forward(LayerId toLayer)
{
    impl->setUpNet();
    impl->forwardLayer(impl->getLayerData(toLayer));
}

void Net::setNetInputs(const std::vector<String> &inputBlobNames)
{
    impl->netInputLayer->setNames(inputBlobNames);
}

void Net::setBlob(String outputName, const Blob &blob)
{
    LayerPin pin = impl->getPinByAlias(outputName);
    if (!pin.valid())
        CV_Error(Error::StsObjectNotFound, "Request blob \"" + outputName + "\" not found");

    LayerData &ld = impl->layers[pin.lid];
    ld.outputBlobs.resize( std::max(pin.oid+1, (int)ld.requiredOutputs.size()) );
    ld.outputBlobs[pin.oid] = blob;
}

Blob Net::getBlob(String outputName)
{
    LayerPin pin = impl->getPinByAlias(outputName);
    if (!pin.valid())
        CV_Error(Error::StsObjectNotFound, "Request blob \"" + outputName + "\" not found");

    LayerData &ld = impl->layers[pin.lid];
    CV_Assert(pin.oid < (int)ld.outputBlobs.size());
    return ld.outputBlobs[pin.oid];
}

Blob Net::getParam(LayerId layer, int numParam)
{
    LayerData &ld = impl->getLayerData(layer);

    std::vector<Blob> &layerBlobs = ld.layerInstance->learnedParams;
    CV_Assert(numParam < (int)layerBlobs.size());
    return layerBlobs[numParam];
}

Importer::~Importer()
{

}

int Layer::inputNameToIndex(String inputName)
{
    return -1;
}

int Layer::outputNameToIndex(String outputName)
{
    return -1;
}

Layer::~Layer()
{

}

//////////////////////////////////////////////////////////////////////////

struct LayerRegister::Impl : public std::map<String, LayerRegister::Constuctor>
{
};

//allocates on load and cleans on exit
Ptr<LayerRegister::Impl> LayerRegister::impl(new LayerRegister::Impl());

void LayerRegister::registerLayer(const String &_type, Constuctor constructor)
{
    String type = _type.toLowerCase();
    Impl::iterator it = impl->find(type);

    if (it != impl->end() && it->second != constructor)
    {
        CV_Error(cv::Error::StsBadArg, "Layer \"" + type + "\" already was registered");
    }

    impl->insert(std::make_pair(type, constructor));
}

void LayerRegister::unregisterLayer(const String &_type)
{
    String type = _type.toLowerCase();
    impl->erase(type);
}

Ptr<Layer> LayerRegister::createLayerInstance(const String &_type, LayerParams& params)
{
    String type = _type.toLowerCase();
    Impl::const_iterator it = LayerRegister::impl->find(type);

    if (it != impl->end())
    {
        return it->second(params);
    }
    else
    {
        return Ptr<Layer>(); //NULL
    }
}

}
}