using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace CSI3304Project1
{
    public class User
    {
        private string userUsername;
        private string userFirstName;
        private string userLastName;
        private string userEmail;
        private string userType;

        public User()
        {
            userUsername = "Username01";
            userFirstName = "First";
            userLastName = "Last";
            userEmail = "email@email.com";
            userType = "user";
        }

        public void setUsername(string inputUsername)
        {
            userUsername = inputUsername;
        }

        public void setFirstName(string inputFirstName)
        {
            userFirstName = inputFirstName;
        }

        public void setLastName(string inputLastName)
        {
            userLastName = inputLastName;
        }

        public void setEmail(string inputEmail)
        {
            userEmail = inputEmail;
        }

        public void setType(string inputType)
        {
            userType = inputType;
        }

        public string getUsername()
        {
            return userUsername;
        }

        public string getFirstName()
        {
            return userFirstName;
        }

        public string getLastName()
        {
            return userLastName;
        }

        public string getEmail()
        {
            return userEmail;
        }

        public string getType()
        {
            return userType;
        }
    }
}
