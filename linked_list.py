
class node:
	def __init__(self, data = None, next = None):
		self.data = data
		self.next = next

	def __str__(self):
		return "Node [" + str(self.data) + "]"

class linked_list:
	def __init__(self):
		self.head = None
		self.tail = None

	def insert(self, values = []):		
		for data in values: 
			self.insert_helper(data)

	def insert_helper(self, data):
		if (self.head == None):
			new_node = node(data)
			self.head = new_node
			self.tail = self.head

		elif self.tail == self.head:
			self.tail = node(data)
			self.head.next = self.tail

		else:					
			last_node = node(data)
			self.tail.next = last_node
			self.tail = last_node

	def show_nodes(self, node = None):
		if (node == None):
			current_node = self.head
		else:
			current_node = node

		while (current_node != None):
			print ("Node [" + str(current_node.data) + "]-->", end = "")
			current_node = current_node.next
		print ("None")

	def reverse(self):
		reversed = None
		next = None
		current_node = self.head

		while (current_node != None):
			next = current_node.next;
			current_node.next = reversed
			reversed = current_node
			current_node = next		

		self.head = reversed

	def delete(self, data):
		prev_node = self.head
		current_node = self.head
		node_to_delete = None

		while (current_node != None):
			if (current_node.data == data):
				node_to_delete = current_node
				break
			
			prev_node = current_node
			current_node = current_node.next

		if (node_to_delete == None):
			print ("Nothing to delete")
		elif node_to_delete == self.head:
			self.head = self.head.next
		elif node_to_delete == self.tail:			
			self.tail = prev_node
			self.tail.next = None
		else:
			prev_node.next = node_to_delete.next


def main():
	ld_list = linked_list()	
	values = [1, 2, 3, 4, 5, 6, 7]
	ld_list.insert(values)
	ld_list.show_nodes()

	ld_list.delete(5)
	ld_list.show_nodes()

if __name__ == "__main__" : main()