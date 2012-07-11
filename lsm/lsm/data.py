# Copyright (C) 2011-2012 Red Hat, Inc.
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
#
# Author: tasleson

from abc import ABCMeta
import json
from json.decoder import WHITESPACE
from common import get_class

class DataEncoder(json.JSONEncoder):
    """
    Custom json encoder for objects derived form ILsmData
    """

    def default(self, my_class):
        if not isinstance(my_class, IData):
            raise ValueError('incorrect class type:' + str(type(my_class)))
        else:
            return my_class.toDict()

class DataDecoder(json.JSONDecoder):
    """
    Custom json decoder for objects derived from ILsmData
    """
    @staticmethod
    def __process_dict(d):
        """
        Processes a dictionary
        """
        rc = {}

        if 'class' in d:
            rc = IData.factory(d)
        else:
            for (k, v) in d.iteritems():
                rc[k] = DataDecoder.__decode(v)

        return rc

    @staticmethod
    def __process_list(l):
        """
        Processes a list
        """
        rc = []
        for elem, value in enumerate(l):
            if type(value) is list:
                rc.append(DataDecoder.__process_list(value))
            elif type(value) is dict:
                rc.append(DataDecoder.__process_dict(value))
            else:
                rc.append(value)
        return rc

    @staticmethod
    def __decode(e):
        """
        Decodes the parsed json
        """
        if type(e) is dict:
            return DataDecoder.__process_dict(e)
        elif type(e) is list:
            return DataDecoder.__process_list(e)
        else:
            return e

    def decode(self, json_string, _w=WHITESPACE.match):
        decoded = json.loads(json_string)
        decoded = DataDecoder.__decode(decoded)
        return decoded


class IData(object):
    """
    Base class functionality of serializable
    classes.
    """
    __metaclass__ = ABCMeta

    def toDict(self):
        """
        Represent the class as a dictionary
        """
        rc = {'class': self.__class__.__name__}

        #If one of the attributes is another IData we will
        #process that too, is there a better way to handle this?
        for (k,v) in self.__dict__.items():
            if isinstance(v, IData):
                rc[k] = v.toDict()
            else:
                rc[k] = v

        return rc

    @staticmethod
    def factory(d):
        """
        Factory for creating the appropriate class given a dictionary.
        This only works for objects that inherit from IData
        """
        if 'class' in d:
            class_name = d['class']
            del d['class']
            c = get_class(__name__ + '.' + class_name)
            i = c(**d)
            return i

    def __str__(self):
        """
        Used for human string representation.
        """
        return str(self.toDict())


class Initiator(IData):
    """
    Represents an initiator.
    """
    (TYPE_OTHER, TYPE_PORT_WWN, TYPE_NODE_WWN, TYPE_HOSTNAME, TYPE_ISCSI) = \
    (1, 2, 3, 4, 5)

    def __init__(self, id, type, name):

        if not name or not len(name):
            name = "Unsupported"

        self.id = id
        self.type = type
        self.name = name


class Volume(IData):
    """
    Represents a volume.
    """

    #Volume status Note: Volumes can have multiple status bits set at same time.
    (STATUS_UNKNOWN, STATUS_OK, STATUS_DEGRADED, STATUS_ERR, STATUS_STARTING,
     STATUS_DORMANT) = (0x0, 0x1, 0x2, 0x4, 0x8, 0x10)

    #Replication types
    (REPLICATE_UNKNOWN, REPLICATE_SNAPSHOT, REPLICATE_CLONE, REPLICATE_COPY,
     REPLICATE_MIRROR_SYNC, REPLICATE_MIRROR_ASYNC) = \
    (-1, 1, 2, 3, 4, 5)

    #Provisioning types
    (PROVISION_UNKNOWN, PROVISION_THIN, PROVISION_FULL, PROVISION_DEFAULT) = \
    ( -1, 1, 2, 3)

    @staticmethod
    def prov_string_to_type(type):
        if type == 'DEFAULT':
            return Volume.PROVISION_DEFAULT
        elif type == "FULL":
            return Volume.PROVISION_FULL
        elif type == "THIN":
            return Volume.PROVISION_THIN
        else:
            return Volume.PROVISION_UNKNOWN

    @staticmethod
    def rep_String_to_type(rt):
        if rt == "SNAPSHOT":
            return Volume.REPLICATE_SNAPSHOT
        elif rt == "CLONE":
            return Volume.REPLICATE_CLONE
        elif rt == "COPY":
            return Volume.REPLICATE_COPY
        elif rt == "MIRROR_SYNC":
            return Volume.REPLICATE_MIRROR_SYNC
        elif rt == "MIRROR_ASYNC":
            return Volume.REPLICATE_MIRROR_ASYNC
        else:
            return Volume.REPLICATE_UNKNOWN

    #Initiator access
    (ACCESS_READ_ONLY, ACCESS_READ_WRITE, ACCESS_NONE) = (1,2,3)

    @staticmethod
    def access_string_to_type(access):
        if access == "RW":
            return Volume.ACCESS_READ_WRITE
        else:
            return Volume.ACCESS_READ_ONLY

    def __init__(self, id, name, vpd83, block_size, num_of_blocks, status,
                 system_id):
        self.id = id
        self.name = name
        self.vpd83 = vpd83
        self.block_size = block_size
        self.num_of_blocks = num_of_blocks
        self.status = status
        self.system_id = system_id

    @property
    def size_bytes(self):
        """
        Volume size in bytes.
        """
        return self.block_size * self.num_of_blocks

    def __str__(self):
        return self.name

class System(IData):
    def __init__(self, id, name):
        self.id = id                # For SMI-S this is the CIM_ComputerSystem->Name
        self.name = name            # For SMI-S this is the CIM_ComputerSystem->ElementName

class Pool(IData):
    """
    Pool specific information
    """
    def __init__(self, id, name, total_space, free_space, system_id):
        self.id = id
        self.name = name
        self.total_space = total_space
        self.free_space = free_space
        self.system_id = system_id

class FileSystem(IData):
    def __init__(self, id, name, total_space, free_space, pool_id, system_id):
        self.id = id
        self.name = name
        self.total_space = total_space
        self.free_space = free_space
        self.pool_id = pool_id
        self.system_id = system_id

class Snapshot(IData):
    def __init__(self, id, name, ts):
        self.id = id
        self.name = name
        self.ts = int(ts)

class NfsExport(IData):
    ANON_UID_GID_NA = 18446744073709551615
    ANON_UID_GID_ERROR = (ANON_UID_GID_NA - 1)

    def __init__(self, id, fs_id, export_path, auth, root, rw, ro, anonuid,
                 anongid, options):
        assert(fs_id is not None)
        assert(export_path is not None)

        self.id = id
        self.fs_id = fs_id          #File system exported
        self.export_path = export_path     #Export path
        self.auth = auth            #Authentication type
        self.root = root            #List of hosts with no_root_squash
        self.rw = rw                #List of hosts with read/write
        self.ro = ro                #List of hosts with read/only
        self.anonuid = anonuid      #uid for anonymous user id
        self.anongid = anongid      #gid for anonymous group id
        self.options = options      #NFS options


class BlockRange(IData):
    def __init__(self, src_block, dest_block, block_count):
        self.src_block = src_block
        self.dest_block = dest_block
        self.block_count = block_count

class AccessGroup(IData):
    def __init__(self, id, name, initiators, system_id = 'NA'):
        self.id = id
        self.name = name
        self.initiators = initiators
        self.system_id = system_id

class Capabilities(IData):

    (
        UNSUPPORTED,        #Not supported
        SUPPORTED,          #Supported
        SUPPORTED_OFFLINE,  #Supported, but only when item is in offline state
        NOT_IMPLEMENTED,    #Not implemented
        UNKNOWN             #Capability not known
    ) = (0 , 1, 2, 3, 4)

    _NUM = 512

    #Array wide
    BLOCK_SUPPORT               = 0          #Array handles block operations
    FS_SUPPORT                  = 1          #Array handles file system

    #Block operations
    VOLUMES                                 = 20
    VOLUME_CREATE                           = 21
    VOLUME_RESIZE                           = 22

    VOLUME_REPLICATE                        = 23
    VOLUME_REPLICATE_CLONE                  = 24
    VOLUME_REPLICATE_COPY                   = 25
    VOLUME_REPLICATE_MIRROR_ASYNC           = 26
    VOLUME_REPLICATE_MIRROR_SYNC            = 27

    VOLUME_COPY_RANGE_BLOCK_SIZE            = 28
    VOLUME_COPY_RANGE                       = 39
    VOLUME_COPY_RANGE_CLONE                 = 30
    VOLUME_COPY_RANGE_COPY                  = 31

    VOLUME_DELETE                           = 33

    VOLUME_ONLINE                           = 34
    VOLUME_OFFLINE                          = 35

    ACCESS_GROUP_GRANT                      = 36
    ACCESS_GROUP_REVOKE                     = 37
    ACCESS_GROUP_LIST                       = 38
    ACCESS_GROUP_CREATE                     = 39
    ACCESS_GROUP_DELETE                     = 40
    ACCESS_GROUP_ADD_INITIATOR              = 41
    ACCESS_GROUP_DEL_INITIATOR              = 42

    VOLUMES_ACCESSIBLE_BY_ACCESS_GROUP      = 43
    ACCESS_GROUPS_GRANTED_TO_VOLUME         = 44

    VOLUME_CHILD_DEPENDENCY                 = 45
    VOLUME_CHILD_DEPENDENCY_RM              = 46

    #File system
    FS                                      = 100
    FS_DELETE                               = 101
    FS_RESIZE                               = 102
    FS_CREATE                               = 103
    FS_CLONE                                = 104
    FILE_CLONE                              = 105
    SNAPSHOTS                               = 106
    SNAPSHOT_CREATE                         = 107
    SNAPSHOT_CREATE_SPECIFIC_FILES          = 108
    SNAPSHOT_DELETE                         = 109
    SNAPSHOT_REVERT                         = 110
    SNAPSHOT_REVERT_SPECIFIC_FILES          = 111
    FS_CHILD_DEPENDENCY                     = 112
    FS_CHILD_DEPENDENCY_RM                  = 113
    FS_CHILD_DEPENDENCY_RM_SPECIFIC_FILES   = 114

    #NFS
    EXPORT_AUTH                             = 120
    EXPORTS                                 = 121
    EXPORT_FS                               = 122
    EXPORT_REMOVE                           = 123

    def toDict(self):
        rc = {'class': self.__class__.__name__,
              'cap': ''.join(['%02x' % b for b in self.cap])}
        return rc

    def __init__(self, cap=None):
        if cap is not None:
            self.cap = bytearray(cap.decode('hex'))
        else:
            self.cap = bytearray(Capabilities._NUM)

    def get(self, capability):
        if capability > len(self.cap):
            return Capabilities.UNKNOWN
        return self.cap[capability]

    def set(self, capability, value):
        self.cap[capability] = value
        return None

    def enable_all(self):
        for i in range(len(self.cap)):
            self.cap[i] = Capabilities.SUPPORTED

if __name__ == '__main__':
    #TODO Need some unit tests that encode/decode all the types with nested
    pass